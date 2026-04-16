#include "touch_xpt2046_v3.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <cstring>
#include <iostream>
#include <chrono>
#include <algorithm>

// XPT2046 Command Bytes
#define XPT2046_CMD_X       0xD0  // X position
#define XPT2046_CMD_Y       0x90  // Y position
#define XPT2046_CMD_Z1      0xB0  // Z1 pressure
#define XPT2046_CMD_Z2      0xC0  // Z2 pressure

TouchXPT2046::TouchXPT2046(const TouchConfig& config)
    : config_(config)
    , spiFd_(-1)
    , lastState_(TOUCH_RELEASED)
    , lastX_(0)
    , lastY_(0)
{
}

TouchXPT2046::~TouchXPT2046() {
    if (spiFd_ >= 0) {
        close(spiFd_);
    }
}

bool TouchXPT2046::init() {
    // Open SPI device
    spiFd_ = open(config_.spiDevice.c_str(), O_RDWR);
    if (spiFd_ < 0) {
        std::cerr << "[TOUCH] Failed to open SPI device: " << config_.spiDevice << std::endl;
        return false;
    }
    
    // Configure SPI mode
    uint8_t mode = SPI_MODE_0;
    if (ioctl(spiFd_, SPI_IOC_WR_MODE, &mode) < 0) {
        std::cerr << "[TOUCH] Failed to set SPI mode" << std::endl;
        close(spiFd_);
        spiFd_ = -1;
        return false;
    }
    
    // Configure bits per word
    uint8_t bits = 8;
    if (ioctl(spiFd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        std::cerr << "[TOUCH] Failed to set SPI bits per word" << std::endl;
        close(spiFd_);
        spiFd_ = -1;
        return false;
    }
    
    // Configure SPI speed
    if (ioctl(spiFd_, SPI_IOC_WR_MAX_SPEED_HZ, &config_.spiSpeed) < 0) {
        std::cerr << "[TOUCH] Failed to set SPI speed" << std::endl;
        close(spiFd_);
        spiFd_ = -1;
        return false;
    }
    
    std::cout << "[TOUCH] XPT2046 initialized on " << config_.spiDevice 
              << " @ " << config_.spiSpeed << " Hz" << std::endl;
    
    return true;
}

bool TouchXPT2046::spiTransfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    struct spi_ioc_transfer tr = {};
    tr.tx_buf = (unsigned long)tx;
    tr.rx_buf = (unsigned long)rx;
    tr.len = len;
    tr.speed_hz = config_.spiSpeed;
    tr.bits_per_word = 8;
    tr.delay_usecs = 0;
    
    return ioctl(spiFd_, SPI_IOC_MESSAGE(1), &tr) >= 0;
}

int TouchXPT2046::readRaw(uint8_t command) {
    uint8_t tx[3] = { command, 0, 0 };
    uint8_t rx[3] = { 0, 0, 0 };
    
    // Add error handling - if SPI fails, return 0 (invalid reading)
    if (!spiTransfer(tx, rx, 3)) {
        // SPI communication failed - return invalid value
        // This will be filtered out by readAveraged
        return 0;
    }
    
    // XPT2046 returns 12-bit value in bits [14:3] of the 16-bit response
    int value = ((rx[1] << 8) | rx[2]) >> 3;
    return value & 0xFFF;  // Mask to 12 bits
}

bool TouchXPT2046::readAveraged(int& x, int& y, int& z, int samples) {
    // Reduce samples for better responsiveness under continuous touch
    // 3 samples is enough for filtering while being fast
    const int MAX_SAMPLES = 3;
    samples = std::min(samples, MAX_SAMPLES);
    
    int sumX = 0, sumY = 0, sumZ = 0;
    int validSamples = 0;
    
    for (int i = 0; i < samples; i++) {
        // Read all channels
        int rawX = readRaw(XPT2046_CMD_X);
        int rawY = readRaw(XPT2046_CMD_Y);
        int z1 = readRaw(XPT2046_CMD_Z1);
        
        // Filter out phantom/invalid readings:
        // 1. Y near max (4095) indicates no touch / invalid reading
        // 2. X near zero with Y maxed = noise
        // 3. Z1 must be >= minimum threshold (real touches have Z1 > 50)
        // 4. Z1 must be < maximum threshold
        bool isValidReading = (rawY < 4000) &&           // Y not maxed out
                             (rawX > 50) &&              // X has real value
                             (z1 >= 50) &&               // Minimum pressure
                             (z1 < config_.pressureThreshold);  // Below max threshold
        
        if (isValidReading) {
            sumX += rawX;
            sumY += rawY;
            sumZ += z1;
            validSamples++;
        }
        
        // Shorter delay for better responsiveness
        if (i < samples - 1) {  // No delay after last sample
            usleep(300);  // Reduced from 500us
        }
    }
    
    if (validSamples >= samples / 2) {  // At least half the samples must be valid
        x = sumX / validSamples;
        y = sumY / validSamples;
        z = sumZ / validSamples;
        return true;
    }
    
    return false;
}

bool TouchXPT2046::isTouched() {
    // Quick check with same filtering as readAveraged
    int rawX = readRaw(XPT2046_CMD_X);
    int rawY = readRaw(XPT2046_CMD_Y);
    int z1 = readRaw(XPT2046_CMD_Z1);
    
    // Filter out phantom readings
    bool isValid = (rawY < 4000) &&           // Y not maxed out
                   (rawX > 50) &&              // X has real value  
                   (z1 >= 50) &&               // Minimum pressure
                   (z1 < config_.pressureThreshold);
    
    return isValid;
}

void TouchXPT2046::getRawPosition(int& x, int& y, int& z) {
    if (!readAveraged(x, y, z)) {
        x = y = z = 0;
    }
}

void TouchXPT2046::calibratePosition(int rawX, int rawY, int& nativeX, int& nativeY) {
    if (config_.numCalPoints == 9) {
        // Bilinear interpolation over the 3×3 calibration grid.
        //
        // Grid layout (row-major, index = row*3 + col):
        //   0(TL) 1(TC) 2(TR)
        //   3(ML) 4(C)  5(MR)
        //   6(BL) 7(BC) 8(BR)
        //
        // Each CalibrationPoint stores both raw ADC values and the native
        // screen coordinates that were displayed there during calibration.
        // We locate the 2×2 cell containing (rawX, rawY) in raw space,
        // then bilinearly interpolate the four corners' native coordinates.

        const auto* pts = config_.calPoints;

        // Detect axis orientation: does rawX grow left→right (column 0→2)?
        int leftColRawX  = pts[0].rawX + pts[3].rawX + pts[6].rawX;
        int rightColRawX = pts[2].rawX + pts[5].rawX + pts[8].rawX;
        bool xIncreasing = (rightColRawX > leftColRawX);

        // Does rawY grow top→bottom (row 0→2)?
        int topRowRawY    = pts[0].rawY + pts[1].rawY + pts[2].rawY;
        int bottomRowRawY = pts[6].rawY + pts[7].rawY + pts[8].rawY;
        bool yIncreasing  = (bottomRowRawY > topRowRawY);

        // Split lines: average raw values of the center column and center row
        int splitRawX = (pts[1].rawX + pts[4].rawX + pts[7].rawX) / 3;
        int splitRawY = (pts[3].rawY + pts[4].rawY + pts[5].rawY) / 3;

        // Select cell in native-grid space (colIdx=0 → left half, rowIdx=0 → top half)
        int colIdx = xIncreasing ? (rawX >= splitRawX ? 1 : 0)
                                 : (rawX <  splitRawX ? 1 : 0);
        int rowIdx = yIncreasing ? (rawY >= splitRawY ? 1 : 0)
                                 : (rawY <  splitRawY ? 1 : 0);

        // Four cell corners (TL/TR/BL/BR in native-grid space)
        int iTL = rowIdx * 3 + colIdx;
        int iTR = rowIdx * 3 + colIdx + 1;
        int iBL = (rowIdx + 1) * 3 + colIdx;
        int iBR = (rowIdx + 1) * 3 + colIdx + 1;

        // Bilinear weights: t_x=0 at TL/BL edge, t_x=1 at TR/BR edge.
        // Averaging the two edges handles slight non-linearity.
        // The formula is robust to axis inversion: if rawX_R < rawX_L the
        // signs cancel and t_x still goes 0→1 across the cell.
        float rawX_L = (pts[iTL].rawX + pts[iBL].rawX) * 0.5f;
        float rawX_R = (pts[iTR].rawX + pts[iBR].rawX) * 0.5f;
        float rawY_T = (pts[iTL].rawY + pts[iTR].rawY) * 0.5f;
        float rawY_B = (pts[iBL].rawY + pts[iBR].rawY) * 0.5f;

        float t_x = (rawX_R != rawX_L) ? (rawX - rawX_L) / (rawX_R - rawX_L) : 0.5f;
        float t_y = (rawY_B != rawY_T) ? (rawY - rawY_T) / (rawY_B - rawY_T) : 0.5f;
        t_x = std::max(0.0f, std::min(1.0f, t_x));
        t_y = std::max(0.0f, std::min(1.0f, t_y));

        // Interpolate native coordinates from the four cell corners
        nativeX = (int)(
            (1 - t_y) * ((1 - t_x) * pts[iTL].nativeX + t_x * pts[iTR].nativeX) +
            t_y       * ((1 - t_x) * pts[iBL].nativeX + t_x * pts[iBR].nativeX));
        nativeY = (int)(
            (1 - t_y) * ((1 - t_x) * pts[iTL].nativeY + t_x * pts[iTR].nativeY) +
            t_y       * ((1 - t_x) * pts[iBL].nativeY + t_x * pts[iBR].nativeY));

    } else {
        // Legacy 4-point linear calibration
        const int CAL_MARGIN = 15;
        const int CAL_WIDTH  = config_.nativeWidth  - (2 * CAL_MARGIN);
        const int CAL_HEIGHT = config_.nativeHeight - (2 * CAL_MARGIN);

        int calX = (rawX - config_.calXMin) * CAL_WIDTH  / (config_.calXMax - config_.calXMin);
        int calY = (rawY - config_.calYMin) * CAL_HEIGHT / (config_.calYMax - config_.calYMin);

        nativeX = calX + CAL_MARGIN;
        nativeY = calY + CAL_MARGIN;
    }

    // Clamp to native sensor bounds
    nativeX = std::max(0, std::min(nativeX, config_.nativeWidth  - 1));
    nativeY = std::max(0, std::min(nativeY, config_.nativeHeight - 1));
}

void TouchXPT2046::transformForDisplay(int nativeX, int nativeY, int& displayX, int& displayY) {
    // Transform from native sensor coordinates to display coordinates
    // Assumes native sensor is portrait (320x480), display can be any orientation
    
    switch (config_.orientation) {
        case 0:   // Landscape (480x320) - 90° CW rotation from portrait
            displayX = nativeY;
            displayY = config_.nativeWidth - 1 - nativeX;
            break;
            
        case 90:  // Portrait (320x480) - Native orientation, no transform
            displayX = nativeX;
            displayY = nativeY;
            break;
            
        case 180: // Landscape inverted (480x320) - 270° CW rotation from portrait  
            displayX = config_.nativeHeight - 1 - nativeY;
            displayY = nativeX;
            break;
            
        case 270: // Portrait inverted (320x480) - 180° rotation from portrait
            displayX = config_.nativeWidth - 1 - nativeX;
            displayY = config_.nativeHeight - 1 - nativeY;
            break;
            
        default:
            // Fallback: no transformation
            displayX = nativeX;
            displayY = nativeY;
            break;
    }
}

bool TouchXPT2046::readTouch(TouchEvent& event) {
    int rawX, rawY, pressure;
    
    // Try to read touch data
    if (!readAveraged(rawX, rawY, pressure)) {
        // No valid touch detected
        if (lastState_ != TOUCH_RELEASED) {
            // Touch was released
            event.x = lastX_;
            event.y = lastY_;
            event.pressure = 0;
            event.state = TOUCH_RELEASED;
            event.valid = true;
            lastState_ = TOUCH_RELEASED;
            
            auto now = std::chrono::steady_clock::now();
            event.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();
            
            return true;
        }
        return false;  // Already released, no event
    }
    
    // Valid touch detected - apply 3-stage coordinate pipeline:
    
    // Stage 1: Calibrate raw ADC to native sensor coordinates
    int nativeX, nativeY;
    calibratePosition(rawX, rawY, nativeX, nativeY);
    
    // Stage 2: Transform native sensor coords to display coords
    int displayX, displayY;
    transformForDisplay(nativeX, nativeY, displayX, displayY);
    
    // Determine touch state
    TouchState newState;
    if (lastState_ == TOUCH_RELEASED) {
        newState = TOUCH_PRESSED;  // New touch
    } else {
        newState = TOUCH_HELD;      // Continuing touch
    }
    
    // Fill event structure with final display coordinates
    event.x = displayX;
    event.y = displayY;
    event.pressure = pressure;
    event.state = newState;
    event.valid = true;
    
    auto now = std::chrono::steady_clock::now();
    event.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    
    // Update last state
    lastState_ = newState;
    lastX_ = displayX;
    lastY_ = displayY;
    
    return true;
}
