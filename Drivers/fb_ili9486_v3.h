#ifndef FB_ILI9486_V3_H
#define FB_ILI9486_V3_H

#include "ili9486.h"
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <memory>
#include <chrono>

#define LCD_WIDTH  320
#define LCD_HEIGHT 480
#define FB_V3_SHM_NAME_0 "/ili9486_fb_v3_0"
#define FB_V3_SHM_NAME_1 "/ili9486_fb_v3_1"
#define FB_V3_SHM_NAME "/ili9486_fb_v3"  // Legacy compatibility
#define FB_V3_STATS_SHM_NAME "/ili9486_fb_v3_stats"

// Thermal state — exposed in FramebufferStats so apps can read it
enum ThermalState : uint32_t {
    THERMAL_NORMAL   = 0,  // < 70°C  — full speed
    THERMAL_WARM     = 1,  // 70–76°C — fps capped at 24
    THERMAL_HOT      = 2,  // 76–82°C — fps capped at 15, SPI halved
    THERMAL_CRITICAL = 3   // > 82°C  — fps capped at 5, SPI quartered
};

// Performance statistics shared between driver and apps
struct FramebufferStats {
    std::atomic<uint64_t> totalFrames;
    std::atomic<uint64_t> droppedFrames;
    std::atomic<uint32_t> currentFps;
    std::atomic<uint32_t> dirtyPixelCount;
    std::atomic<uint32_t> avgUpdateTimeUs;
    std::atomic<bool> driverActive;
    std::atomic<bool> frameInProgress;  // Set by app during draw, prevents driver sampling
    std::atomic<uint32_t> activeBufferIndex;  // 0 or 1 - which buffer is currently displayed

    // Per-buffer completion flags - set by driver when buffer processing complete
    std::atomic<bool> buffer0Done;
    std::atomic<bool> buffer1Done;

    // Thermal throttling — readable by apps
    std::atomic<uint32_t> cpuTempMilliC;      // e.g. 72500 = 72.5°C
    std::atomic<uint32_t> thermalState;        // ThermalState enum value

    // App-requested SPI speed override.
    // Write a non-zero Hz value to request a runtime clock change;
    // write 0 to restore the driver's configured default.
    // The driver applies it on the next update cycle, respecting thermal throttle.
    std::atomic<uint32_t> requestedSpiSpeedHz;
};

// Dirty rectangle with automatic merging
struct DirtyRegion {
    uint16_t x, y, width, height;
    bool dirty;

    void reset() {
        x = y = width = height = 0;
        dirty = false;
    }

    void merge(uint16_t nx, uint16_t ny, uint16_t nw, uint16_t nh) {
        if (!dirty) {
            x = nx; y = ny; width = nw; height = nh;
            dirty = true;
        } else {
            uint16_t x2 = std::max(x + width, nx + nw);
            uint16_t y2 = std::max(y + height, ny + nh);
            x = std::min(x, nx);
            y = std::min(y, ny);
            width = x2 - x;
            height = y2 - y;
        }
    }
};

class ILI9486FramebufferV3 {
public:
    ILI9486FramebufferV3(const std::string& spiDevice, int dcPin, int rstPin, uint32_t spiSpeed = 32000000);
    ~ILI9486FramebufferV3();

    // Core functionality
    bool init();
    void startUpdateLoop();
    void stopUpdateLoop();

    // Manual update trigger (for compatibility)
    void updateDisplay();

    // Advanced features
    void enableAutoDirtyDetection(bool enable = true);
    void enableFrameSkipping(bool enable = true);
    void setDirtyThreshold(float threshold = 0.01f); // Percentage of screen
    void setSPISpeed(uint32_t speedHz); // Set SPI clock (32MHz safe, 80MHz tested)
    void resetStats();

    // Display settings
    void setOrientation(int degrees);  // 0, 90, 180, 270
    void setMirrorX(bool mirror);
    void setMirrorY(bool mirror);

    // Accessors
    uint16_t* getFramebuffer() { return framebuffer0; }  // Legacy - returns buffer 0
    uint16_t* getBackBuffer();   // Get buffer for writing (non-active)
    uint16_t* getFrontBuffer();  // Get buffer currently displaying (active)
    FramebufferStats* getStats() { return stats; }
    
    // Buffer swapping
    void swapBuffers();  // Atomically swap front/back buffers

protected:
    // Hardware interface
    ILI9486 lcd;

    // Shared memory framebuffer (mmap) - DOUBLE BUFFERED
    int shm_fd0, shm_fd1;
    int stats_shm_fd;
    uint16_t* framebuffer0;  // Buffer 0
    uint16_t* framebuffer1;  // Buffer 1
    FramebufferStats* stats;
    size_t fbSize;

    // Double buffering for dirty detection (one previous frame per buffer)
    std::vector<uint16_t> previousFrame0;
    std::vector<uint16_t> previousFrame1;
    // Snapshot buffer: atomic copy of active framebuffer taken at the start of each
    // detection cycle — detection and copy both work from this, never from the live buffer
    std::vector<uint16_t> snapshotBuf;
    // Double-buffered transfer buffers: detection writes to one, SPI reads from the other
    std::vector<uint16_t> transferBuf[2];

    // Threading
    std::thread updateThread;
    std::thread spiThread;
    std::atomic<bool> running;
    std::atomic<bool> frameReady;
    std::mutex frameMutex;
    std::condition_variable frameCV;          // spiThread waits here for a new frame
    std::condition_variable frameConsumedCV;  // updateLoop waits here until spiThread clears frameReady

    // Dirty region tracking
    std::atomic<bool> autoDirtyDetection;
    std::atomic<bool> frameSkippingEnabled;
    float dirtyThreshold;
    DirtyRegion currentDirty;

    // Performance tracking
    std::chrono::steady_clock::time_point lastFrameTime;
    std::chrono::steady_clock::time_point fpsCounterStart;
    int frameCount;
    uint32_t frameCountForFps;
    int pendingBufIdx;  // Protected by frameMutex — which transferBuf the SPI thread reads next

    // Thermal throttling
    uint32_t nominalSpiSpeed;                               // Effective nominal speed (may be overridden by app)
    uint32_t configSpiSpeed_;                               // Original speed from settings.conf, never changed
    uint32_t lastReqHz_;                                    // Last requestedSpiSpeedHz value we acted on
    std::atomic<uint32_t> thermalTargetFrameUs;             // Current frame interval (µs), updated by checkThermal()
    std::chrono::steady_clock::time_point lastThermalCheck;

    // Internal methods
    bool createSharedMemory();
    void destroySharedMemory();

    // Main update loop (runs in separate thread)
    void updateLoop();

    // SPI transfer loop (runs in separate thread)
    void spiTransferLoop();

    // Dirty detection — snapshot and activeIdx both fixed by caller before calling
    bool detectDirtyRegion(DirtyRegion& region, const uint16_t* snapshot, uint32_t activeIdx);

    // Thermal management — reads /sys/class/thermal, adjusts fps cap and SPI speed
    void checkThermal();

    // Performance monitoring
    void updateFpsCounter();
    void recordFrameTime(uint32_t microseconds);
};

// C API for easy integration
extern "C" {
    // Simple C interface
    void* fb_v3_create(const char* spiDevice, int dcPin, int rstPin);
    int fb_v3_init(void* fb);
    void fb_v3_start(void* fb);
    void fb_v3_stop(void* fb);
    void fb_v3_destroy(void* fb);
    uint16_t* fb_v3_get_buffer(void* fb);
    void fb_v3_update(void* fb);

    // Get shared memory directly (for external apps)
    uint16_t* fb_v3_map_shared_memory();  // Legacy - maps buffer 0
    void fb_v3_unmap_shared_memory(uint16_t* addr);
    
    // Double buffer API
    uint16_t* fb_v3_get_back_buffer();
    uint16_t* fb_v3_get_front_buffer();
    void fb_v3_swap_buffers();
    FramebufferStats* fb_v3_get_stats();
}

#endif // FB_ILI9486_V3_H
