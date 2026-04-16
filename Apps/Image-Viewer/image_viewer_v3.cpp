#include <iostream>
#include <cstring>
#include <cstdio>
#include <signal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Framebuffer dimensions - ALWAYS 320x480 in memory regardless of orientation
// The display controller rotates the output based on MADCTL register
#define FB_WIDTH   320
#define FB_HEIGHT  480
#define FB_V3_SHM_NAME_0 "/ili9486_fb_v3_0"
#define FB_V3_SHM_NAME_1 "/ili9486_fb_v3_1"
#define FB_V3_SHM_NAME "/ili9486_fb_v3"
#define FB_V3_STATS_SHM_NAME "/ili9486_fb_v3_stats"

// Physical display dimensions - Set to LANDSCAPE mode (orientation=0)
// This is the "natural" orientation where the display is wider than tall
#define LCD_WIDTH  480
#define LCD_HEIGHT 320

// Stats structure (must match driver - fb_ili9486_v3.h FramebufferStats)
struct FramebufferStats {
    std::atomic<uint64_t> totalFrames;
    std::atomic<uint64_t> droppedFrames;
    std::atomic<uint32_t> currentFps;
    std::atomic<uint32_t> dirtyPixelCount;
    std::atomic<uint32_t> avgUpdateTimeUs;
    std::atomic<bool> driverActive;
    std::atomic<bool> frameInProgress;
    std::atomic<uint32_t> activeBufferIndex;
    std::atomic<bool> buffer0Done;
    std::atomic<bool> buffer1Done;
};

enum ScaleMode {
    FIT,      // Scale to fit inside display (maintain aspect ratio, letterbox)
    FILL      // Scale to fill display (maintain aspect ratio, crop edges)
};

enum ImageOrientation {
    PORTRAIT,   // Height > Width
    LANDSCAPE,  // Width > Height
    SQUARE      // Width ≈ Height
};

enum DisplayOrientation {
    DISPLAY_LANDSCAPE,  // 480x320
    DISPLAY_PORTRAIT    // 320x480
};

static const char* PID_FILE = "/tmp/image_viewer_v3.pid";

static pid_t checkExistingInstance() {
    FILE* f = fopen(PID_FILE, "r");
    if (!f) return 0;
    pid_t pid = 0;
    bool ok = (fscanf(f, "%d", &pid) == 1);
    fclose(f);
    if (ok && pid > 0 && kill(pid, 0) == 0) return pid;
    unlink(PID_FILE);
    return 0;
}

static void writePidFile() {
    FILE* f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}

static volatile bool running = true;
static void handleSignal(int) { unlink(PID_FILE); running = false; }

// Map a specific buffer by index (0 or 1) - for consistent display
uint16_t* mapSpecificBuffer(int bufferIndex) {
    const char* shmName = (bufferIndex == 0) ? FB_V3_SHM_NAME_0 : FB_V3_SHM_NAME_1;
    
    int fd = shm_open(shmName, O_RDWR, 0666);
    if (fd < 0) {
        std::cerr << "[IMG_V3] Failed to open buffer " << bufferIndex << ": " << shmName << std::endl;
        return nullptr;
    }

    void* addr = mmap(nullptr, FB_WIDTH * FB_HEIGHT * 2,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        std::cerr << "[IMG_V3] Failed to mmap buffer " << bufferIndex << std::endl;
        return nullptr;
    }

    std::cout << "[IMG_V3] Mapped buffer " << bufferIndex << ": " << shmName << " (fixed buffer)" << std::endl;
    return (uint16_t*)addr;
}

// Map front buffer (for single image display - shows immediately)
uint16_t* mapFrontBuffer() {
    // Get stats to determine active buffer
    int stats_fd = shm_open(FB_V3_STATS_SHM_NAME, O_RDONLY, 0666);
    if (stats_fd < 0) {
        std::cerr << "[IMG_V3] Failed to open stats. Is fb_main_v3 running?" << std::endl;
        return nullptr;
    }
    
    FramebufferStats* stats = (FramebufferStats*)mmap(nullptr, sizeof(FramebufferStats),
                                                        PROT_READ, MAP_SHARED, stats_fd, 0);
    close(stats_fd);
    
    if (stats == MAP_FAILED) {
        std::cerr << "[IMG_V3] Failed to mmap stats" << std::endl;
        return nullptr;
    }
    
    // Front buffer is the ACTIVE buffer (currently displaying)
    uint32_t activeIdx = stats->activeBufferIndex.load();
    munmap(stats, sizeof(FramebufferStats));
    
    const char* shmName = (activeIdx == 0) ? FB_V3_SHM_NAME_0 : FB_V3_SHM_NAME_1;
    
    int fd = shm_open(shmName, O_RDWR, 0666);
    if (fd < 0) {
        std::cerr << "[IMG_V3] Failed to open front buffer: " << shmName << std::endl;
        return nullptr;
    }

    void* addr = mmap(nullptr, FB_WIDTH * FB_HEIGHT * 2,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        std::cerr << "[IMG_V3] Failed to mmap front buffer" << std::endl;
        return nullptr;
    }

    std::cout << "[IMG_V3] Mapped front buffer: " << shmName << " (displays immediately)" << std::endl;
    return (uint16_t*)addr;
}

// Map back buffer (for double buffering)
uint16_t* mapBackBuffer() {
    // Get stats to determine active buffer
    int stats_fd = shm_open(FB_V3_STATS_SHM_NAME, O_RDONLY, 0666);
    if (stats_fd < 0) {
        std::cerr << "[IMG_V3] Failed to open stats. Is fb_main_v3 running?" << std::endl;
        return nullptr;
    }
    
    FramebufferStats* stats = (FramebufferStats*)mmap(nullptr, sizeof(FramebufferStats),
                                                        PROT_READ, MAP_SHARED, stats_fd, 0);
    close(stats_fd);
    
    if (stats == MAP_FAILED) {
        std::cerr << "[IMG_V3] Failed to mmap stats" << std::endl;
        return nullptr;
    }
    
    // Back buffer is the NON-active buffer
    uint32_t activeIdx = stats->activeBufferIndex.load();
    munmap(stats, sizeof(FramebufferStats));
    
    const char* shmName = (activeIdx == 0) ? FB_V3_SHM_NAME_1 : FB_V3_SHM_NAME_0;
    
    int fd = shm_open(shmName, O_RDWR, 0666);
    if (fd < 0) {
        std::cerr << "[IMG_V3] Failed to open back buffer: " << shmName << std::endl;
        return nullptr;
    }

    void* addr = mmap(nullptr, FB_WIDTH * FB_HEIGHT * 2,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        std::cerr << "[IMG_V3] Failed to mmap back buffer" << std::endl;
        return nullptr;
    }

    std::cout << "[IMG_V3] Mapped back buffer: " << shmName << " (for preloading)" << std::endl;
    return (uint16_t*)addr;
}

void unmapFramebuffer(uint16_t* fb) {
    if (fb) munmap(fb, FB_WIDTH * FB_HEIGHT * 2);
}

// Swap buffers (make back buffer visible)
void swapBuffers() {
    int stats_fd = shm_open(FB_V3_STATS_SHM_NAME, O_RDWR, 0666);
    if (stats_fd < 0) return;
    
    FramebufferStats* stats = (FramebufferStats*)mmap(nullptr, sizeof(FramebufferStats),
                                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                                        stats_fd, 0);
    close(stats_fd);
    
    if (stats != MAP_FAILED) {
        uint32_t current = stats->activeBufferIndex.load();
        stats->activeBufferIndex.store(1 - current);
        std::cout << "[IMG_V3] Swapped buffers: " << current << " -> " << (1 - current) << std::endl;
        munmap(stats, sizeof(FramebufferStats));
    }
}

// Map stats shared memory
FramebufferStats* mapStats() {
    int fd = shm_open(FB_V3_STATS_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) return nullptr;

    void* addr = mmap(nullptr, sizeof(FramebufferStats),
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) return nullptr;
    return (FramebufferStats*)addr;
}

void unmapStats(FramebufferStats* stats) {
    if (stats) munmap(stats, sizeof(FramebufferStats));
}



// Convert RGB888 to RGB565 with proper rounding
inline uint16_t rgb888ToRgb565(uint8_t r, uint8_t g, uint8_t b) {
    // Use rounding for better gradients
    uint16_t r5 = (r * 31 + 128) / 255;
    uint16_t g6 = (g * 63 + 128) / 255;
    uint16_t b5 = (b * 31 + 128) / 255;

    // RGB565 format: red in bits 15-11, green in 10-5, blue in 4-0
    return (r5 << 11) | (g6 << 5) | b5;
}

// Clear screen to color
void clearScreen(uint16_t* fb, uint16_t color = 0x0000) {
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        fb[i] = color;
    }
}

// Detect image orientation
ImageOrientation detectImageOrientation(int imgW, int imgH) {
    float aspectRatio = (float)imgW / imgH;

    if (aspectRatio > 1.1f) {
        return LANDSCAPE;  // Width significantly larger than height
    } else if (aspectRatio < 0.9f) {
        return PORTRAIT;   // Height significantly larger than width
    } else {
        return SQUARE;     // Roughly equal dimensions
    }
}

// Detect display orientation
DisplayOrientation detectDisplayOrientation() {
    if (LCD_WIDTH > LCD_HEIGHT) {
        return DISPLAY_LANDSCAPE;
    } else {
        return DISPLAY_PORTRAIT;
    }
}

// Bilinear interpolation - sample image with smooth filtering
inline void samplePixelGammaCorrect(uint8_t* imgData, int imgW, int imgH, int channels,
                                     float x, float y, uint8_t& r, uint8_t& g, uint8_t& b) {
    x = std::max(0.0f, std::min((float)(imgW - 1), x));
    y = std::max(0.0f, std::min((float)(imgH - 1), y));

    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = std::min(x0 + 1, imgW - 1);
    int y1 = std::min(y0 + 1, imgH - 1);

    float fx = x - x0;
    float fy = y - y0;
    float fx1 = 1.0f - fx;
    float fy1 = 1.0f - fy;

    // Get 4 surrounding pixels
    int idx00 = (y0 * imgW + x0) * channels;
    int idx10 = (y0 * imgW + x1) * channels;
    int idx01 = (y1 * imgW + x0) * channels;
    int idx11 = (y1 * imgW + x1) * channels;

    // Convert to linear space (gamma decode)
    auto toLinear = [](uint8_t v) -> float {
        float normalized = v / 255.0f;
        return powf(normalized, 2.2f);  // Decode gamma
    };

    // Interpolate in linear space
    float r_lin = toLinear(imgData[idx00 + 0]) * fx1 * fy1 +
                  toLinear(imgData[idx10 + 0]) * fx  * fy1 +
                  toLinear(imgData[idx01 + 0]) * fx1 * fy +
                  toLinear(imgData[idx11 + 0]) * fx  * fy;

    float g_lin = toLinear(imgData[idx00 + 1]) * fx1 * fy1 +
                  toLinear(imgData[idx10 + 1]) * fx  * fy1 +
                  toLinear(imgData[idx01 + 1]) * fx1 * fy +
                  toLinear(imgData[idx11 + 1]) * fx  * fy;

    float b_lin = toLinear(imgData[idx00 + 2]) * fx1 * fy1 +
                  toLinear(imgData[idx10 + 2]) * fx  * fy1 +
                  toLinear(imgData[idx01 + 2]) * fx1 * fy +
                  toLinear(imgData[idx11 + 2]) * fx  * fy;

    // Convert back to sRGB (gamma encode)
    auto toSRGB = [](float v) -> uint8_t {
        v = std::max(0.0f, std::min(1.0f, v));
        return (uint8_t)(powf(v, 1.0f/2.2f) * 255.0f + 0.5f);
    };

    r = toSRGB(r_lin);
    g = toSRGB(g_lin);
    b = toSRGB(b_lin);
}

// Global display orientation (declared in main)
extern int g_displayOrientation;

// Display image with intelligent orientation handling (like a phone)
void displayImage(uint16_t* fb, FramebufferStats* stats,
                  uint8_t* imgData, int imgW, int imgH, int channels,
                  ScaleMode mode) {

    // Render into a temporary buffer WITHOUT holding frameInProgress.
    // This keeps the driver's dirty-detection thread unblocked during the
    // (potentially slow) per-pixel render loop.
    // Only the final memcpy into the real framebuffer runs under the flag.
    std::vector<uint16_t> tmpBuf(FB_WIDTH * FB_HEIGHT, 0);

    // Determine actual LCD dimensions based on display orientation
    int actualLcdWidth = (g_displayOrientation == 0) ? 480 : 320;
    int actualLcdHeight = (g_displayOrientation == 0) ? 320 : 480;

    // Detect image orientation
    ImageOrientation imgOrient = detectImageOrientation(imgW, imgH);
    DisplayOrientation dispOrient = (actualLcdWidth > actualLcdHeight) ? DISPLAY_LANDSCAPE : DISPLAY_PORTRAIT;

    // NO ROTATION - just scale images to fit in their natural orientation
    // This allows any image to fit in any display orientation like a photo gallery
    bool needsRotation = false;

    // Use original image dimensions for scaling (no swapping)
    int effectiveImgW = imgW;
    int effectiveImgH = imgH;

    // Calculate scaling (initialize to avoid compiler warnings)
    float scaleX = 1.0f, scaleY = 1.0f, scale = 1.0f;
    int drawW = effectiveImgW, drawH = effectiveImgH;
    int offsetX = 0, offsetY = 0;

    switch (mode) {
        case FIT:
            // Scale to fit (letterbox) - maintain aspect ratio
            scaleX = (float)actualLcdWidth / effectiveImgW;
            scaleY = (float)actualLcdHeight / effectiveImgH;
            scale = std::min(scaleX, scaleY);
            drawW = (int)(effectiveImgW * scale);
            drawH = (int)(effectiveImgH * scale);
            offsetX = (actualLcdWidth - drawW) / 2;   // Center horizontally
            offsetY = (actualLcdHeight - drawH) / 2;  // Center vertically
            break;

        case FILL:
            // Scale to fill (crop) - maintain aspect ratio, crop edges
            scaleX = (float)actualLcdWidth / effectiveImgW;
            scaleY = (float)actualLcdHeight / effectiveImgH;
            scale = std::max(scaleX, scaleY);
            drawW = (int)(effectiveImgW * scale);
            drawH = (int)(effectiveImgH * scale);
            offsetX = (actualLcdWidth - drawW) / 2;   // Center horizontally
            offsetY = (actualLcdHeight - drawH) / 2;  // Center vertically
            break;
    }

    std::cout << "[IMG_V3] ===== Image Info =====" << std::endl;
    std::cout << "[IMG_V3] Driver orientation: " << g_displayOrientation << "°" << std::endl;
    std::cout << "[IMG_V3] Image: " << imgW << "x" << imgH
              << " (" << (imgOrient == PORTRAIT ? "Portrait" : imgOrient == LANDSCAPE ? "Landscape" : "Square") << ")" << std::endl;
    std::cout << "[IMG_V3] Display: " << actualLcdWidth << "x" << actualLcdHeight
              << " (" << (dispOrient == DISPLAY_PORTRAIT ? "Portrait" : "Landscape") << ")" << std::endl;
    std::cout << "[IMG_V3] Needs rotation: " << (needsRotation ? "YES" : "NO") << std::endl;
    std::cout << "[IMG_V3] Effective size (after rotation): " << effectiveImgW << "x" << effectiveImgH << std::endl;
    std::cout << "[IMG_V3] ScaleX: " << scaleX << ", ScaleY: " << scaleY << ", Final scale: " << scale << std::endl;
    std::cout << "[IMG_V3] Draw size: " << drawW << "x" << drawH
              << " at offset (" << offsetX << "," << offsetY << ")" << std::endl;
    std::cout << "[IMG_V3] ====================" << std::endl;

    // Render image with bilinear interpolation
    // LCD coordinates are the physical display (depends on driver orientation)
    // FB coordinates are the framebuffer memory (320x480 portrait always)
    // The display controller handles the rotation via MADCTL
    for (int lcdY = 0; lcdY < actualLcdHeight; lcdY++) {
        for (int lcdX = 0; lcdX < actualLcdWidth; lcdX++) {
            // Map LCD coordinates to framebuffer coordinates based on driver orientation
            int fbX, fbY;
            if (g_displayOrientation == 0) {
                // Landscape: LCD(480x320) -> FB(320x480) with 90° CW rotation
                // LCD(lcdX, lcdY) -> FB(lcdY, 479-lcdX)
                fbX = lcdY;
                fbY = 479 - lcdX;
            } else {
                // Portrait: LCD(320x480) -> FB(320x480) direct 1:1
                fbX = lcdX;
                fbY = lcdY;
            }

            // Calculate draw coordinates (relative to centered/scaled image)
            int drawX = lcdX - offsetX;
            int drawY = lcdY - offsetY;

            // Skip if outside image bounds
            if (drawX < 0 || drawX >= drawW || drawY < 0 || drawY >= drawH) {
                continue; // Leave as black (already cleared)
            }

            // Map screen coordinate to scaled image coordinate (with subpixel precision)
            float scaledX = (float)drawX / scale;
            float scaledY = (float)drawY / scale;

            // Apply inverse rotation to map back to original image coordinates
            float srcX, srcY;
            if (needsRotation) {
                // We're displaying the image rotated 90° CW
                // scaledX, scaledY are in rotated coordinate space
                // Original portrait: imgW x imgH (e.g., 724x1280)
                // After rotation: effectiveImgW x effectiveImgH (e.g., 1280x724)
                //
                // For 90° CW rotation:
                // rotated(x, y) should come from original(imgW-1-y, x)
                //
                // So the inverse mapping is:
                // srcX = imgW - 1 - scaledY
                // srcY = scaledX
                srcX = imgW - 1 - scaledY;
                srcY = scaledX;
            } else {
                srcX = scaledX;
                srcY = scaledY;
            }

            // Bounds check - ensure we're not reading outside the original image
            if (srcX < 0 || srcX >= imgW || srcY < 0 || srcY >= imgH) {
                continue; // Skip this pixel
            }

            // TESTING: Use nearest-neighbor sampling (no interpolation) to avoid color bleeding
            int srcPixelX = (int)srcX;  // Truncate instead of round
            int srcPixelY = (int)srcY;

            // Strict bounds check
            if (srcPixelX < 0 || srcPixelX >= imgW || srcPixelY < 0 || srcPixelY >= imgH) {
                continue;
            }

            // Direct pixel fetch
            int idx = (srcPixelY * imgW + srcPixelX) * channels;
            uint8_t r = imgData[idx];
            uint8_t g = (channels >= 2) ? imgData[idx + 1] : r;
            uint8_t b = (channels >= 3) ? imgData[idx + 2] : r;

            // Direct conversion to RGB565: red in bits 15-11, green in 10-5, blue in 4-0
            uint16_t r5 = r >> 3;
            uint16_t g6 = g >> 2;
            uint16_t b5 = b >> 3;

            tmpBuf[fbY * FB_WIDTH + fbX] = (r5 << 11) | (g6 << 5) | b5;
        }
    }

    // All rendering is done. Now grab frameInProgress for the minimal
    // window needed to copy the finished pixels into the real framebuffer.
    if (stats) {
        stats->frameInProgress.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    memcpy(fb, tmpBuf.data(), FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));

    if (stats) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        stats->frameInProgress.store(false, std::memory_order_seq_cst);
    }
}

// Global display orientation - can be overridden by command line
int g_displayOrientation = 0; // 0=landscape(480x320), 90=portrait(320x480)

int main(int argc, char** argv) {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    pid_t existing = checkExistingInstance();
    if (existing) {
        std::cerr << "[IMG_V3] Already running (PID " << existing << "). Kill it first:  kill " << existing << std::endl;
        return 1;
    }
    writePidFile();

    std::cout << R"(
╔═══════════════════════════════════════════════╗
║   Image Viewer V3 - Auto Scale & Display      ║
║   Supports PNG, JPG, BMP, TGA                 ║
╚═══════════════════════════════════════════════╝
)" << std::endl;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <image_file> [options]" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Options:" << std::endl;
        std::cerr << "  fit           - Scale to fit (default, letterbox)" << std::endl;
        std::cerr << "  fill          - Scale to fill (crop edges)" << std::endl;
        std::cerr << "  -fb0          - Use buffer 0 (default, consistent display)" << std::endl;
        std::cerr << "  -fb1          - Use buffer 1 (alternative fixed buffer)" << std::endl;
        std::cerr << "  -double       - Use double buffering (for slideshows/video)" << std::endl;
        std::cerr << "  0 | 90        - Display orientation (landscape/portrait)" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Examples:" << std::endl;
        std::cerr << "  " << argv[0] << " image.jpg fill           # Fill screen, buffer 0" << std::endl;
        std::cerr << "  " << argv[0] << " image.jpg -fb0 fill      # Explicit buffer 0" << std::endl;
        std::cerr << "  " << argv[0] << " image.jpg -double fill   # Double buffering mode" << std::endl;
        return 1;
    }

    const char* filename = argv[1];
    ScaleMode mode = FIT;
    int bufferMode = 0;  // 0=buffer0, 1=buffer1, 2=double buffering

    // Parse arguments
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "fit") {
            mode = FIT;
        } else if (arg == "fill") {
            mode = FILL;
        } else if (arg == "-fb0") {
            bufferMode = 0;
        } else if (arg == "-fb1") {
            bufferMode = 1;
        } else if (arg == "-double") {
            bufferMode = 2;
        } else if (arg == "0") {
            g_displayOrientation = 0;
        } else if (arg == "90") {
            g_displayOrientation = 90;
        } else {
            std::cerr << "[IMG_V3] Unknown option: " << arg << std::endl;
            return 1;
        }
    }

    // Load image
    std::cout << "[IMG_V3] Loading image: " << filename << std::endl;
    int imgW, imgH, channels;
    uint8_t* imgData = stbi_load(filename, &imgW, &imgH, &channels, 0);

    if (!imgData) {
        std::cerr << "[IMG_V3] Failed to load image: " << stbi_failure_reason() << std::endl;
        return 1;
    }

    std::cout << "[IMG_V3] Image channels: " << channels << std::endl;

    // Debug: Sample center pixel to see RGB values
    int centerIdx = (imgH/2 * imgW + imgW/2) * channels;
    std::cout << "[IMG_V3] Center pixel RGB: "
              << (int)imgData[centerIdx] << ", "
              << (int)imgData[centerIdx+1] << ", "
              << (int)imgData[centerIdx+2] << std::endl;

    std::cout << "[IMG_V3] Image loaded: " << imgW << "x" << imgH
              << " (" << channels << " channels)" << std::endl;

    // Connect to framebuffer based on buffer mode
    uint16_t* fb = nullptr;
    bool needsSwap = false;
    
    switch (bufferMode) {
        case 0:
            std::cout << "[IMG_V3] Using BUFFER 0 (fixed, consistent)" << std::endl;
            fb = mapSpecificBuffer(0);
            break;
        case 1:
            std::cout << "[IMG_V3] Using BUFFER 1 (fixed, alternative)" << std::endl;
            fb = mapSpecificBuffer(1);
            break;
        case 2:
            std::cout << "[IMG_V3] Using DOUBLE BUFFERING (back buffer + swap)" << std::endl;
            fb = mapBackBuffer();
            needsSwap = true;
            break;
    }
    
    if (!fb) {
        stbi_image_free(imgData);
        return 1;
    }

    FramebufferStats* stats = mapStats();
    if (!stats) {
        std::cerr << "[IMG_V3] Warning: Could not access stats" << std::endl;
    }

    std::cout << "[IMG_V3] Connected! Displaying image..." << std::endl;
    std::cout << "[IMG_V3] Scale mode: " << (mode == FIT ? "FIT (letterbox)" : "FILL (crop)") << std::endl;

    // Display image
    displayImage(fb, stats, imgData, imgW, imgH, channels, mode);
    
    // Swap if using double buffering
    if (needsSwap) {
        std::cout << "[IMG_V3] Swapping buffers..." << std::endl;
        swapBuffers();
    }
    
    std::cout << "[IMG_V3] Image displayed on screen!" << std::endl;
    std::cout << "[IMG_V3] Press Ctrl+C to close or run again for next image." << std::endl;

    std::cout << "[IMG_V3] Done." << std::endl;
    // Cleanup
    stbi_image_free(imgData);
    unmapFramebuffer(fb);
    if (stats) unmapStats(stats);

    std::cout << "[IMG_V3] Exited." << std::endl;
    unlink(PID_FILE);
    return 0;
}
