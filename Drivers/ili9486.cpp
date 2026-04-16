#include "ili9486.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/spi/spidev.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <thread>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <pigpio.h>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

#define LCD_WIDTH  320
#define LCD_HEIGHT 480

ILI9486::ILI9486(const std::string& spiDevice, int dcPin, int rstPin, uint32_t spiSpeed)
    : spi_fd(-1), dcPin(dcPin), rstPin(rstPin), spiDevice(spiDevice), spiSpeed(spiSpeed),
      orientation(0), mirrorX(false), mirrorY(false),
      dmaEnabled(false), doubleBuffering(false), refreshRate(60),
      pigpioHandle(-1), isDaemonMode(false) {

    // Check if pigpio daemon PID file exists (indicates daemon is running)
    bool daemonRunning = (access("/var/run/pigpio.pid", F_OK) == 0);

    // Try to initialize pigpio in standalone mode
    pigpioHandle = gpioInitialise();
    if (pigpioHandle < 0) {
        std::cerr << "\n╔════════════════════════════════════════════════════════════════╗" << std::endl;
        std::cerr << "║ ERROR: Failed to initialize pigpio!                           ║" << std::endl;
        std::cerr << "╚════════════════════════════════════════════════════════════════╝" << std::endl;

        if (daemonRunning) {
            std::cerr << "\n⚠️  Detected: pigpio daemon (or fb_main_v3/touch_main_v3) is running!" << std::endl;
            std::cerr << "\nDirect Hardware apps cannot run while daemons are active." << std::endl;
            std::cerr << "\n┌─ Solution ─────────────────────────────────────────────────┐" << std::endl;
            std::cerr << "│ Stop the daemons first:                                    │" << std::endl;
            std::cerr << "│   sudo killall fb_main_v3 touch_main_v3 pigpiod           │" << std::endl;
            std::cerr << "│                                                            │" << std::endl;
            std::cerr << "│ Then run this app:                                         │" << std::endl;
            std::cerr << "│   sudo " << spiDevice << "                                          │" << std::endl;
            std::cerr << "└────────────────────────────────────────────────────────────┘" << std::endl;
        } else {
            std::cerr << "\nPossible causes:" << std::endl;
            std::cerr << "  • Not running with sudo (required for GPIO/SPI access)" << std::endl;
            std::cerr << "  • Hardware not accessible" << std::endl;
            std::cerr << "\nTry: sudo " << spiDevice << std::endl;
        }
        std::cerr << "════════════════════════════════════════════════════════════════\n" << std::endl;
    } else {
        isDaemonMode = false;
        std::cout << "[pigpio] Initialized standalone mode (version: " << pigpioHandle << ")" << std::endl;
    }
    initBuffers();
}

ILI9486::~ILI9486() {
    closeSPI();
    // Only terminate if we successfully initialized
    if (pigpioHandle >= 0 && !isDaemonMode) {
        gpioTerminate();
    }
}

void ILI9486::initBuffers() {
    size_t bufferSize = LCD_WIDTH * LCD_HEIGHT;
    backBuffer.resize(bufferSize, 0);
    frontBuffer.resize(bufferSize, 0);
    clearDirty();
}

void ILI9486::clearDirty() {
    dirtyRect.dirty = false;
    dirtyRect.x = dirtyRect.y = dirtyRect.width = dirtyRect.height = 0;
}

void ILI9486::markDirty(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    if (!dirtyRect.dirty) {
        dirtyRect.x = x;
        dirtyRect.y = y;
        dirtyRect.width = width;
        dirtyRect.height = height;
        dirtyRect.dirty = true;
    } else {
        // Expand dirty rectangle to include new area
        uint16_t x2 = std::max(dirtyRect.x + dirtyRect.width, x + width);
        uint16_t y2 = std::max(dirtyRect.y + dirtyRect.height, y + height);
        dirtyRect.x = std::min(dirtyRect.x, x);
        dirtyRect.y = std::min(dirtyRect.y, y);
        dirtyRect.width = x2 - dirtyRect.x;
        dirtyRect.height = y2 - dirtyRect.y;
    }
}

void ILI9486::enableDMA(bool enable) {
    dmaEnabled = enable;
    std::cout << "[LCD] DMA " << (enable ? "enabled" : "disabled") << std::endl;
}

void ILI9486::setDoubleBuffering(bool enable) {
    doubleBuffering = enable;
    std::cout << "[LCD] Double buffering " << (enable ? "enabled" : "disabled") << std::endl;
}

void ILI9486::setRefreshRate(int fps) {
    refreshRate = fps;
    std::cout << "[LCD] Refresh rate set to " << fps << " FPS" << std::endl;
}

void ILI9486::setOrientation(int degrees) {
    orientation = degrees;
    std::cout << "[LCD] Orientation set to " << degrees << " degrees" << std::endl;
    applyMADCTL();
}

void ILI9486::setMirrorX(bool mirror) {
    mirrorX = mirror;
    std::cout << "[LCD] Mirror X " << (mirror ? "enabled" : "disabled") << std::endl;
    applyMADCTL();
}

void ILI9486::setMirrorY(bool mirror) {
    mirrorY = mirror;
    std::cout << "[LCD] Mirror Y " << (mirror ? "enabled" : "disabled") << std::endl;
    applyMADCTL();
}

void ILI9486::applyMADCTL() {
    // MADCTL (Memory Access Control) - ILI9486 register 0x36
    // Bits: MY MX MV ML BGR MH 0 0
    // MY (bit 7): Row Address Order (vertical flip)
    // MX (bit 6): Column Address Order (horizontal flip)
    // MV (bit 5): Row/Column Exchange (rotation)
    // ML (bit 4): Vertical Refresh Order
    // BGR (bit 3): RGB-BGR Order (1 = BGR, 0 = RGB)
    // MH (bit 2): Horizontal Refresh Order

    uint8_t madctl = 0x00; // BGR bit CLEARED - display expects RGB565 format

    // Apply orientation
    switch (orientation) {
        case 0:   // Portrait (320x480) - default
            madctl |= 0x40; // MX bit
            break;
        case 90:  // Landscape (480x320)
            madctl |= 0x20; // MV bit
            break;
        case 180: // Portrait inverted
            madctl |= 0x80; // MY bit
            break;
        case 270: // Landscape inverted
            madctl |= 0xE0; // MY | MX | MV bits
            break;
        default:
            std::cerr << "[LCD] Invalid orientation: " << orientation << ", using 0" << std::endl;
            madctl |= 0x40; // Default to portrait
            break;
    }

    // Apply mirroring
    if (mirrorX) {
        madctl ^= 0x40; // Toggle MX bit
    }
    if (mirrorY) {
        madctl ^= 0x80; // Toggle MY bit
    }

    // Write to display
    writeCommand(0x36);
    writeData(madctl);

    std::cout << "[LCD] MADCTL = 0x" << std::hex << (int)madctl << std::dec << std::endl;
}

void ILI9486::swapBuffers() {
    if (!doubleBuffering) return;
    
    std::swap(backBuffer, frontBuffer);
    markDirty(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void ILI9486::updateRegion(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    
    width = std::min(width, (uint16_t)(LCD_WIDTH - x));
    height = std::min(height, (uint16_t)(LCD_HEIGHT - y));
    
    setAddressWindow(x, y, x + width - 1, y + height - 1);
    
    // Optimized region update with larger buffer
    const size_t BUF_SIZE = 1024;
    uint8_t buffer[BUF_SIZE * 2];
    size_t pixelsSent = 0;
    
    gpioSetMode(dcPin, PI_OUTPUT);
    gpioWrite(dcPin, 1);
    
    for (uint16_t row = y; row < y + height; row++) {
        for (uint16_t col = x; col < x + width; col++) {
            uint16_t pixel = doubleBuffering ? backBuffer[row * LCD_WIDTH + col] : 0x0000;
            
            size_t bufIndex = (pixelsSent % BUF_SIZE) * 2;
            buffer[bufIndex] = (uint8_t)(pixel >> 8);
            buffer[bufIndex + 1] = (uint8_t)(pixel & 0xFF);
            pixelsSent++;
            
            // Send buffer when full or at end of row
            if ((pixelsSent % BUF_SIZE == 0) || (col == x + width - 1)) {
                size_t bytesToSend = (pixelsSent % BUF_SIZE == 0) ? BUF_SIZE * 2 : (pixelsSent % BUF_SIZE) * 2;
                int ret = write(spi_fd, buffer, bytesToSend);
                if (ret != (int)bytesToSend) {
                    std::cerr << "[SPI] Failed to write region data" << std::endl;
                    break;
                }
            }
        }
    }
}

void ILI9486::updateDirtyRegion() {
    if (!dirtyRect.dirty) return;

    updateRegion(dirtyRect.x, dirtyRect.y, dirtyRect.width, dirtyRect.height);
    clearDirty();
}

// Partial update from a PACKED (contiguous) buffer — row r starts at packedBuf[r * width].
// Detection copies dirty rows contiguously so this function gets a flat array it can
// byte-swap and send in one ioctl call rather than many batched write() calls.
void ILI9486::updateRegionFromBuffer(const uint16_t* packedBuf, uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    if (!packedBuf || x >= LCD_WIDTH || y >= LCD_HEIGHT) return;

    width  = std::min(width,  (uint16_t)(LCD_WIDTH  - x));
    height = std::min(height, (uint16_t)(LCD_HEIGHT - y));

    setAddressWindow(x, y, x + width - 1, y + height - 1);

    gpioSetMode(dcPin, PI_OUTPUT);
    gpioWrite(dcPin, 1);

    size_t total = (size_t)width * height;

    // Pre-allocated byte buffer — same lifetime as the static spiBuf in copyFramebuffer.
    // 307KB covers the worst case (full screen via this path).
    static uint8_t* spiBuf = []() {
        auto* p = new uint8_t[LCD_WIDTH * LCD_HEIGHT * 2];
        madvise(p, LCD_WIDTH * LCD_HEIGHT * 2, MADV_SEQUENTIAL);
        return p;
    }();

#ifdef __aarch64__
    // NEON: byte-swap 8 pixels per iteration (MSB first for SPI)
    size_t i = 0;
    for (; i + 8 <= total; i += 8) {
        uint16x8_t pixels = vld1q_u16(packedBuf + i);
        uint8x8_t hi = vmovn_u16(vshrq_n_u16(pixels, 8));
        uint8x8_t lo = vmovn_u16(vandq_u16(pixels, vdupq_n_u16(0xFF)));
        uint8x8x2_t interleaved = { hi, lo };
        vst2_u8(spiBuf + i * 2, interleaved);
    }
    size_t remaining = total - i;
    for (size_t j = 0; j < remaining; j++) {
        spiBuf[(i + j) * 2]     = (uint8_t)(packedBuf[i + j] >> 8);
        spiBuf[(i + j) * 2 + 1] = (uint8_t)(packedBuf[i + j] & 0xFF);
    }
#else
    for (size_t i = 0; i < total; i++) {
        spiBuf[i * 2]     = (uint8_t)(packedBuf[i] >> 8);
        spiBuf[i * 2 + 1] = (uint8_t)(packedBuf[i] & 0xFF);
    }
#endif

    // Single ioctl — one kernel round-trip for the entire dirty region
    struct spi_ioc_transfer spi_transfer = {};
    spi_transfer.tx_buf      = (unsigned long)spiBuf;
    spi_transfer.len         = total * 2;
    spi_transfer.speed_hz    = spiSpeed;
    spi_transfer.bits_per_word = 8;

    int ret = ioctl(spi_fd, SPI_IOC_MESSAGE(1), &spi_transfer);
    if (ret < 0) {
        std::cerr << "[SPI] Failed to write region data (ioctl)" << std::endl;
    }
}

bool ILI9486::openSPI() {
    std::cout << "[SPI] Opening SPI device: " << spiDevice << std::endl;
    spi_fd = open(spiDevice.c_str(), O_RDWR);
    if (spi_fd < 0) {
        std::cerr << "[SPI] Failed to open SPI device: " << strerror(errno) << std::endl;
        return false;
    }
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0) std::cerr << "[SPI] Failed to set mode\n";
    if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) std::cerr << "[SPI] Failed to set bits per word\n";
    if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spiSpeed) < 0) std::cerr << "[SPI] Failed to set speed\n";

    // Read back actual speed to verify
    uint32_t actual_speed = 0;
    if (ioctl(spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &actual_speed) >= 0) {
        std::cout << "[SPI] SPI device opened at " << actual_speed/1000000 << " MHz (requested " << spiSpeed/1000000 << " MHz)" << std::endl;
        if (actual_speed != spiSpeed) {
            std::cout << "[SPI] Note: Actual speed differs from requested (hardware limitation)" << std::endl;
        }
    } else {
        std::cout << "[SPI] SPI device opened at " << spiSpeed/1000000 << " MHz" << std::endl;
    }
    return true;
}

void ILI9486::closeSPI() {
    if (spi_fd >= 0) close(spi_fd);
    spi_fd = -1;
}

void ILI9486::reset() {
    std::cout << "[LCD] Resetting LCD..." << std::endl;
    gpioSetMode(rstPin, PI_OUTPUT);
    gpioWrite(rstPin, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gpioWrite(rstPin, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "[LCD] Reset complete." << std::endl;
}

void ILI9486::writeCommand(uint8_t cmd) {
    gpioSetMode(dcPin, PI_OUTPUT);
    gpioWrite(dcPin, 0); // Command
    int ret = write(spi_fd, &cmd, 1);
    if (ret != 1) std::cerr << "[SPI] Failed to write command 0x" << std::hex << (int)cmd << std::dec << std::endl;
}
void ILI9486::writeData(uint8_t data) {
    gpioSetMode(dcPin, PI_OUTPUT);
    gpioWrite(dcPin, 1); // Data
    int ret = write(spi_fd, &data, 1);
    if (ret != 1) std::cerr << "[SPI] Failed to write data 0x" << std::hex << (int)data << std::dec << std::endl;
}
void ILI9486::writeData16(uint16_t color) {
    // No conversion - pass through RGB565
    uint8_t data[2] = {
        (uint8_t)(color >> 8),
        (uint8_t)(color & 0xFF)
    };
    gpioSetMode(dcPin, PI_OUTPUT);
    gpioWrite(dcPin, 1);
    int ret = write(spi_fd, data, 2);
    if (ret != 2) std::cerr << "[SPI] Failed to write 16-bit data" << std::endl;
}

void ILI9486::setAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    writeCommand(0x2A); // Column addr set
    writeData(x0 >> 8); writeData(x0 & 0xFF);
    writeData(x1 >> 8); writeData(x1 & 0xFF);
    writeCommand(0x2B); // Row addr set
    writeData(y0 >> 8); writeData(y0 & 0xFF);
    writeData(y1 >> 8); writeData(y1 & 0xFF);
    writeCommand(0x2C); // Write to RAM
}

bool ILI9486::init() {
    std::cout << "[INIT] Setting up GPIOs with pigpio..." << std::endl;
    gpioSetMode(dcPin, PI_OUTPUT);
    gpioSetMode(rstPin, PI_OUTPUT);
    reset();
    if (!openSPI()) return false;
    std::cout << "[INIT] Starting ILI9486 initialization sequence..." << std::endl;
    // Full ILI9486 init sequence (based on datasheet and open-source drivers)
    writeCommand(0x11); // Sleep out
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    writeCommand(0x3A); // Interface Pixel Format
    writeData(0x55);    // 16 bits/pixel

    // Memory Access Control - Apply orientation and mirror settings
    applyMADCTL();

    writeCommand(0xC2); // Power Control 3
    writeData(0x44);

    writeCommand(0xC5); // VCOM Control
    writeData(0x00);
    writeData(0x00);
    writeData(0x00);
    writeData(0x00);

    writeCommand(0xE0); // Positive Gamma Control
    writeData(0x0F); writeData(0x1F); writeData(0x1C); writeData(0x0C);
    writeData(0x0F); writeData(0x08); writeData(0x48); writeData(0x98);
    writeData(0x37); writeData(0x0A); writeData(0x13); writeData(0x04);
    writeData(0x11); writeData(0x0D); writeData(0x00);

    writeCommand(0xE1); // Negative Gamma Control
    writeData(0x0F); writeData(0x32); writeData(0x2E); writeData(0x0B);
    writeData(0x0D); writeData(0x05); writeData(0x47); writeData(0x75);
    writeData(0x37); writeData(0x06); writeData(0x10); writeData(0x03);
    writeData(0x24); writeData(0x20); writeData(0x00);

    writeCommand(0x20); // Display Inversion Off
    writeCommand(0x29); // Display ON
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::cout << "[INIT] ILI9486 initialization complete." << std::endl;
    return true;
}

void ILI9486::drawPixel(uint16_t x, uint16_t y, uint16_t color) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    
    if (doubleBuffering) {
        backBuffer[y * LCD_WIDTH + x] = color;
        markDirty(x, y, 1, 1);
    } else {
        setAddressWindow(x, y, x, y);
        writeData16(color);
    }
}

void ILI9486::fastFillScreen(uint16_t color) {
    setAddressWindow(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    size_t numPixels = LCD_WIDTH * LCD_HEIGHT;
    // Prepare a large buffer for fast SPI transfer
    const size_t BUF_PIXELS = 1024; // Tune for your Pi's memory
    uint8_t buf[BUF_PIXELS * 2];
    for (size_t i = 0; i < BUF_PIXELS; ++i) {
        buf[2 * i] = (uint8_t)(color >> 8);
        buf[2 * i + 1] = (uint8_t)(color & 0xFF);
    }
    gpioSetMode(dcPin, PI_OUTPUT);
    gpioWrite(dcPin, 1);
    size_t sent = 0;
    while (sent < numPixels) {
        size_t toSend = std::min(BUF_PIXELS, numPixels - sent);
        int ret = write(spi_fd, buf, toSend * 2);
        if (ret != (int)(toSend * 2)) {
            std::cerr << "[SPI] Failed to write fast fill data" << std::endl;
            break;
        }
        sent += toSend;
    }
}

void ILI9486::copyFramebuffer(const uint16_t* buffer, size_t bufferSize) {
    if (!buffer || bufferSize != LCD_WIDTH * LCD_HEIGHT) {
        std::cerr << "[LCD] Invalid framebuffer data" << std::endl;
        return;
    }

    setAddressWindow(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

    gpioSetMode(dcPin, PI_OUTPUT);
    gpioWrite(dcPin, 1);

#ifdef __aarch64__
    // NEON TURBO MODE: Pack pixels big-endian and send in one ioctl (no color conversion —
    // MADCTL BGR=0 so display already expects RGB565)
    // Static buffer allocated once (avoids per-frame allocation overhead)
    static uint8_t* spiBuf = []() {
        auto* p = new uint8_t[LCD_WIDTH * LCD_HEIGHT * 2];  // 307KB, allocated once
        madvise(p, LCD_WIDTH * LCD_HEIGHT * 2, MADV_SEQUENTIAL);
        return p;
    }();

    size_t i = 0;

    // NEON: Process 8 pixels at a time — pack to big-endian bytes
    for (; i + 8 <= bufferSize; i += 8) {
        uint16x8_t pixels = vld1q_u16(buffer + i);

        // Pack as big-endian bytes (MSB first)
        uint8x8_t high_bytes = vmovn_u16(vshrq_n_u16(pixels, 8));
        uint8x8_t low_bytes  = vmovn_u16(vandq_u16(pixels, vdupq_n_u16(0xFF)));

        uint8x8x2_t interleaved;
        interleaved.val[0] = high_bytes;
        interleaved.val[1] = low_bytes;
        vst2_u8(spiBuf + i * 2, interleaved);
    }

    // Handle remaining pixels (scalar)
    size_t remaining = bufferSize - i;
    for (size_t j = 0; j < remaining; j++) {
        spiBuf[(i + j) * 2]     = (uint8_t)(buffer[i + j] >> 8);
        spiBuf[(i + j) * 2 + 1] = (uint8_t)(buffer[i + j] & 0xFF);
    }

    // Send entire frame in ONE ioctl call (307KB transfer)
    struct spi_ioc_transfer spi_transfer = {};
    spi_transfer.tx_buf = (unsigned long)spiBuf;
    spi_transfer.len = bufferSize * 2;
    spi_transfer.speed_hz = spiSpeed;
    spi_transfer.bits_per_word = 8;

    int ret = ioctl(spi_fd, SPI_IOC_MESSAGE(1), &spi_transfer);
    if (ret < 0) {
        std::cerr << "[SPI] Failed to write full-frame data (ioctl)" << std::endl;
    }
#else
    // Fallback for non-ARM64: scalar conversion
    const size_t BUF_PIXELS = 2048;
    uint8_t spiBuf[BUF_PIXELS * 2];

    size_t pixelsRemaining = bufferSize;
    size_t offset = 0;

    while (pixelsRemaining > 0) {
        size_t batchSize = (pixelsRemaining < BUF_PIXELS) ? pixelsRemaining : BUF_PIXELS;

        for (size_t i = 0; i < batchSize; i++) {
            uint16_t color = buffer[offset + i];
            // No conversion - pass through RGB565
            spiBuf[i * 2] = (uint8_t)(color >> 8);
            spiBuf[i * 2 + 1] = (uint8_t)(color & 0xFF);
        }

        struct spi_ioc_transfer spi_transfer = {};
        spi_transfer.tx_buf = (unsigned long)spiBuf;
        spi_transfer.len = batchSize * 2;
        spi_transfer.speed_hz = spiSpeed;  // Use configured SPI speed
        spi_transfer.bits_per_word = 8;

        int ret = ioctl(spi_fd, SPI_IOC_MESSAGE(1), &spi_transfer);
        if (ret < 0) {
            std::cerr << "[SPI] Failed to write framebuffer data (ioctl)" << std::endl;
            break;
        }

        offset += batchSize;
        pixelsRemaining -= batchSize;
    }
#endif
}

void ILI9486::fastCopyFramebuffer(const uint16_t* buffer, size_t bufferSize) {
    if (!buffer || bufferSize != LCD_WIDTH * LCD_HEIGHT) {
        return; // Silent error for performance
    }

    // Just call the optimized copyFramebuffer - it's already using NEON
    copyFramebuffer(buffer, bufferSize);
} 