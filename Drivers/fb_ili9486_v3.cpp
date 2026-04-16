#include "fb_ili9486_v3.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <pthread.h>
#include <sched.h>
#include <arm_neon.h>  // RISKY: Use NEON SIMD for speed
#include <pigpio.h>

#define FB_SIZE (LCD_WIDTH * LCD_HEIGHT * 2)


ILI9486FramebufferV3::ILI9486FramebufferV3(const std::string& spiDevice, int dcPin, int rstPin, uint32_t spiSpeed)
    : lcd(spiDevice, dcPin, rstPin, spiSpeed), shm_fd0(-1), shm_fd1(-1), stats_shm_fd(-1),
      framebuffer0(nullptr), framebuffer1(nullptr), stats(nullptr), fbSize(FB_SIZE),
      running(false), frameReady(false), autoDirtyDetection(true),
      frameSkippingEnabled(true), dirtyThreshold(0.01f), frameCount(0), frameCountForFps(0),
      pendingBufIdx(0), nominalSpiSpeed(spiSpeed), configSpiSpeed_(spiSpeed), lastReqHz_(0),
      thermalTargetFrameUs(33333) {

    // Pre-allocate buffers - one previous frame per buffer
    previousFrame0.resize(LCD_WIDTH * LCD_HEIGHT, 0);
    previousFrame1.resize(LCD_WIDTH * LCD_HEIGHT, 0);
    snapshotBuf.resize(LCD_WIDTH * LCD_HEIGHT, 0);
    transferBuf[0].resize(LCD_WIDTH * LCD_HEIGHT, 0);
    transferBuf[1].resize(LCD_WIDTH * LCD_HEIGHT, 0);
    currentDirty.reset();

    std::cout << "[FB_V3] Created with DOUBLE BUFFERING + NEON SIMD support" << std::endl;
}

ILI9486FramebufferV3::~ILI9486FramebufferV3() {
    stopUpdateLoop();
    destroySharedMemory();
}

bool ILI9486FramebufferV3::init() {
    std::cout << "[FB_V3] Initializing..." << std::endl;

    if (!lcd.init()) {
        std::cerr << "[FB_V3] Failed to initialize LCD" << std::endl;
        return false;
    }

    if (!createSharedMemory()) {
        std::cerr << "[FB_V3] Failed to create shared memory" << std::endl;
        return false;
    }

    // Initialize stats
    if (stats) {
        stats->totalFrames = 0;
        stats->droppedFrames = 0;
        stats->currentFps = 0;
        stats->dirtyPixelCount = 0;
        stats->avgUpdateTimeUs = 0;
        stats->driverActive = true;
        stats->frameInProgress = false;
        stats->activeBufferIndex = 0;
        stats->buffer0Done = true;
        stats->buffer1Done = true;
        stats->cpuTempMilliC        = 0;
        stats->thermalState         = THERMAL_NORMAL;
        stats->requestedSpiSpeedHz  = 0;
    }

    lastThermalCheck = std::chrono::steady_clock::now();

    // IMPORTANT: Clear LCD to black on startup (even with dirty detection enabled)
    std::cout << "[FB_V3] Clearing screen to black..." << std::endl;
    lcd.fastFillScreen(0x0000);  // Clear to black

    // Initialize previousFrame buffers to match the cleared screen
    memset(previousFrame0.data(), 0, fbSize);
    memset(previousFrame1.data(), 0, fbSize);

    std::cout << "[FB_V3] Initialized successfully" << std::endl;
    return true;
}

bool ILI9486FramebufferV3::createSharedMemory() {
    // RISK #2: Use POSIX shared memory for zero-copy access - DOUBLE BUFFERED
    std::cout << "[FB_V3] Creating double-buffered shared memory..." << std::endl;

    // Create framebuffer 0 shared memory
    shm_unlink(FB_V3_SHM_NAME_0); // Clean up any existing
    shm_fd0 = shm_open(FB_V3_SHM_NAME_0, O_CREAT | O_RDWR, 0666);
    if (shm_fd0 < 0) {
        std::cerr << "[FB_V3] Failed to create shared memory 0: " << strerror(errno) << std::endl;
        return false;
    }

    if (fchmod(shm_fd0, 0666) < 0) {
        std::cerr << "[FB_V3] Warning: Failed to set shared memory 0 permissions" << std::endl;
    }

    if (ftruncate(shm_fd0, fbSize) < 0) {
        std::cerr << "[FB_V3] Failed to set shared memory 0 size" << std::endl;
        return false;
    }

    framebuffer0 = (uint16_t*)mmap(nullptr, fbSize, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, shm_fd0, 0);
    if (framebuffer0 == MAP_FAILED) {
        std::cerr << "[FB_V3] Failed to mmap framebuffer 0" << std::endl;
        return false;
    }

    // Create framebuffer 1 shared memory
    shm_unlink(FB_V3_SHM_NAME_1);
    shm_fd1 = shm_open(FB_V3_SHM_NAME_1, O_CREAT | O_RDWR, 0666);
    if (shm_fd1 < 0) {
        std::cerr << "[FB_V3] Failed to create shared memory 1: " << strerror(errno) << std::endl;
        return false;
    }

    if (fchmod(shm_fd1, 0666) < 0) {
        std::cerr << "[FB_V3] Warning: Failed to set shared memory 1 permissions" << std::endl;
    }

    if (ftruncate(shm_fd1, fbSize) < 0) {
        std::cerr << "[FB_V3] Failed to set shared memory 1 size" << std::endl;
        return false;
    }

    framebuffer1 = (uint16_t*)mmap(nullptr, fbSize, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, shm_fd1, 0);
    if (framebuffer1 == MAP_FAILED) {
        std::cerr << "[FB_V3] Failed to mmap framebuffer 1" << std::endl;
        return false;
    }

    // Create stats shared memory
    shm_unlink(FB_V3_STATS_SHM_NAME);
    stats_shm_fd = shm_open(FB_V3_STATS_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (stats_shm_fd < 0) {
        std::cerr << "[FB_V3] Failed to create stats shared memory" << std::endl;
        return false;
    }

    if (fchmod(stats_shm_fd, 0666) < 0) {
        std::cerr << "[FB_V3] Warning: Failed to set stats memory permissions" << std::endl;
    }

    if (ftruncate(stats_shm_fd, sizeof(FramebufferStats)) < 0) {
        std::cerr << "[FB_V3] Failed to set stats memory size" << std::endl;
        return false;
    }

    stats = (FramebufferStats*)mmap(nullptr, sizeof(FramebufferStats),
                                     PROT_READ | PROT_WRITE, MAP_SHARED,
                                     stats_shm_fd, 0);
    if (stats == MAP_FAILED) {
        std::cerr << "[FB_V3] Failed to mmap stats" << std::endl;
        return false;
    }

    // Initialize both framebuffers to black
    memset(framebuffer0, 0, fbSize);
    memset(framebuffer1, 0, fbSize);

    // Remove any stale legacy SHM segment from a previous run.
    // The legacy name (/ili9486_fb_v3) is NOT kept alive — it was never synced with
    // framebuffer0 so apps writing to it got no display output. Use _0/_1 instead.
    shm_unlink(FB_V3_SHM_NAME);

    std::cout << "[FB_V3] Double-buffered shared memory created:" << std::endl;
    std::cout << "[FB_V3]   Buffer 0: " << FB_V3_SHM_NAME_0 << std::endl;
    std::cout << "[FB_V3]   Buffer 1: " << FB_V3_SHM_NAME_1 << std::endl;
    std::cout << "[FB_V3]   Legacy  : " << FB_V3_SHM_NAME << " (links to buffer 0)" << std::endl;
    std::cout << "[FB_V3] Framebuffer size: " << fbSize << " bytes each (" << LCD_WIDTH << "x" << LCD_HEIGHT << ")" << std::endl;
    std::cout << "[FB_V3] Apps can mmap these directly for zero-copy access!" << std::endl;

    return true;
}

void ILI9486FramebufferV3::destroySharedMemory() {
    if (framebuffer0 && framebuffer0 != MAP_FAILED) {
        munmap(framebuffer0, fbSize);
        framebuffer0 = nullptr;
    }

    if (framebuffer1 && framebuffer1 != MAP_FAILED) {
        munmap(framebuffer1, fbSize);
        framebuffer1 = nullptr;
    }

    if (stats && stats != MAP_FAILED) {
        if (stats) stats->driverActive = false;
        munmap(stats, sizeof(FramebufferStats));
        stats = nullptr;
    }

    if (shm_fd0 >= 0) {
        close(shm_fd0);
        shm_unlink(FB_V3_SHM_NAME_0);
        shm_fd0 = -1;
    }

    if (shm_fd1 >= 0) {
        close(shm_fd1);
        shm_unlink(FB_V3_SHM_NAME_1);
        shm_fd1 = -1;
    }

    if (stats_shm_fd >= 0) {
        close(stats_shm_fd);
        shm_unlink(FB_V3_STATS_SHM_NAME);
        stats_shm_fd = -1;
    }

    // Clean up legacy link
    shm_unlink(FB_V3_SHM_NAME);
}

void ILI9486FramebufferV3::startUpdateLoop() {
    if (running) return;

    running = true;
    fpsCounterStart = std::chrono::steady_clock::now();
    frameCountForFps = 0;

    // RISK #3: TWO threads - one for dirty detection, one for SPI
    updateThread = std::thread(&ILI9486FramebufferV3::updateLoop, this);
    spiThread = std::thread(&ILI9486FramebufferV3::spiTransferLoop, this);

    // Pin threads to dedicated cores to avoid scheduler interference
    // Pi 4 has 4 cores (0-3): leave 0-1 for OS/apps, use 2-3 for driver threads
    cpu_set_t cpus;
    CPU_ZERO(&cpus); CPU_SET(3, &cpus);
    pthread_setaffinity_np(spiThread.native_handle(), sizeof(cpu_set_t), &cpus);
    CPU_ZERO(&cpus); CPU_SET(2, &cpus);
    pthread_setaffinity_np(updateThread.native_handle(), sizeof(cpu_set_t), &cpus);

    // Elevate SPI thread to real-time scheduling to minimise transfer jitter
    struct sched_param sp = {};
    sp.sched_priority = 50;
    if (pthread_setschedparam(spiThread.native_handle(), SCHED_FIFO, &sp) != 0) {
        std::cerr << "[FB_V3] Warning: SCHED_FIFO failed for SPI thread (need root?)" << std::endl;
    }

    std::cout << "[FB_V3] Update loop started (dual-threaded pipeline, core affinity set)" << std::endl;
}

void ILI9486FramebufferV3::stopUpdateLoop() {
    if (!running) return;

    running = false;
    frameCV.notify_all();
    frameConsumedCV.notify_all();  // unblock updateLoop if waiting for spiThread

    if (updateThread.joinable()) updateThread.join();
    if (spiThread.joinable()) spiThread.join();

    std::cout << "[FB_V3] Update loop stopped" << std::endl;
}

// Thermal management — reads CPU temp and adjusts fps cap + SPI speed.
// Called periodically from updateLoop() (every ~2 seconds, not every frame).
// 3°C hysteresis on downward transitions prevents state flapping.
void ILI9486FramebufferV3::checkThermal() {
    FILE* f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return;

    int32_t rawTemp = 0;
    bool ok = (fscanf(f, "%d", &rawTemp) == 1);
    fclose(f);
    if (!ok || rawTemp <= 0) return;

    if (stats) stats->cpuTempMilliC = (uint32_t)rawTemp;

    // Current state for hysteresis — read without a lock (uint32 read is atomic on ARM)
    uint32_t prevState = stats ? stats->thermalState.load() : THERMAL_NORMAL;

    // Thresholds going UP are strict; going DOWN add 3°C hysteresis
    ThermalState newState;
    uint32_t targetUs;
    uint32_t spiHz;

    if (rawTemp >= 82000) {
        newState  = THERMAL_CRITICAL;
        targetUs  = 200000;                              // 5 fps
        spiHz     = std::max(nominalSpiSpeed / 4, 8000000u);
    } else if (rawTemp >= 76000 || (prevState >= THERMAL_HOT && rawTemp >= 73000)) {
        newState  = THERMAL_HOT;
        targetUs  = 66666;                               // 15 fps
        spiHz     = std::max(nominalSpiSpeed / 2, 8000000u);
    } else if (rawTemp >= 70000 || (prevState >= THERMAL_WARM && rawTemp >= 67000)) {
        newState  = THERMAL_WARM;
        targetUs  = 41666;                               // 24 fps
        spiHz     = nominalSpiSpeed;
    } else {
        newState  = THERMAL_NORMAL;
        targetUs  = 33333;                               // 30 fps
        spiHz     = nominalSpiSpeed;
    }

    // Only log + apply changes when state actually changes
    if (newState != (ThermalState)prevState) {
        const char* names[] = { "NORMAL", "WARM", "HOT", "CRITICAL" };
        const uint32_t fpsCaps[] = { 30, 24, 15, 5 };
        if (newState > (ThermalState)prevState) {
            std::cerr << "[THERMAL] " << rawTemp / 1000 << "°C — throttling to "
                      << fpsCaps[newState] << " fps (state: " << names[newState] << ")" << std::endl;
        } else {
            std::cout << "[THERMAL] " << rawTemp / 1000 << "°C — recovering to "
                      << fpsCaps[newState] << " fps (state: " << names[newState] << ")" << std::endl;
        }
        if (stats) stats->thermalState = (uint32_t)newState;
        lcd.setSPISpeed(spiHz);
    }

    thermalTargetFrameUs.store(targetUs, std::memory_order_relaxed);
}

// Two-pass dirty detection — operates entirely on the caller-supplied snapshot,
// never on the live shared-memory framebuffer
bool ILI9486FramebufferV3::detectDirtyRegion(DirtyRegion& region, const uint16_t* snapshot, uint32_t activeIdx) {
    if (!autoDirtyDetection) {
        region.x = 0;
        region.y = 0;
        region.width = LCD_WIDTH;
        region.height = LCD_HEIGHT;
        region.dirty = true;
        return true;
    }

    const uint16_t* activeFramebuffer = snapshot;
    uint16_t* activePrevious = (activeIdx == 0) ? previousFrame0.data() : previousFrame1.data();

    // V2 GPU TWO-PASS ALGORITHM:
    // Pass 1: Fast scan every 2nd pixel to get rough bounds
    // Pass 2: Detailed scan only in affected area to find exact bounds
    region.reset();

    int minX = LCD_WIDTH, maxX = 0;
    int minY = LCD_HEIGHT, maxY = 0;
    bool hasChanges = false;

    // PASS 1: Fast scan - check every 2nd pixel to catch thin lines
    for (int y = 0; y < LCD_HEIGHT; y += 2) {
        for (int x = 0; x < LCD_WIDTH; x += 2) {
            int idx = y * LCD_WIDTH + x;
            if (activeFramebuffer[idx] != activePrevious[idx]) {
                hasChanges = true;
                // Quick bounds update
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    if (!hasChanges) {
        return false; // Nothing changed
    }

    // PASS 2: Detailed scan in the affected area
    // First expand the search area slightly before detailed scan
    const int searchPad = 4;
    int searchMinX = std::max(0, minX - searchPad);
    int searchMaxX = std::min(LCD_WIDTH - 1, maxX + searchPad);
    int searchMinY = std::max(0, minY - searchPad);
    int searchMaxY = std::min(LCD_HEIGHT - 1, maxY + searchPad);

    // Reset bounds for detailed scan
    minX = LCD_WIDTH;
    maxX = 0;
    minY = LCD_HEIGHT;
    maxY = 0;

    // Detailed scan in the affected region (every pixel)
    for (int y = searchMinY; y <= searchMaxY; y++) {
        for (int x = searchMinX; x <= searchMaxX; x++) {
            int idx = y * LCD_WIDTH + x;
            if (activeFramebuffer[idx] != activePrevious[idx]) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    // Final safety padding to avoid edge artifacts on thin geometry.
    // 2px is enough — Pass 2 already expands with searchPad=4 before the precise scan.
    const int borderPad = 2;
    minX = std::max(0, minX - borderPad);
    maxX = std::min(LCD_WIDTH - 1, maxX + borderPad);
    minY = std::max(0, minY - borderPad);
    maxY = std::min(LCD_HEIGHT - 1, maxY + borderPad);

    region.x = minX;
    region.y = minY;
    region.width = maxX - minX + 1;
    region.height = maxY - minY + 1;
    region.dirty = true;

    // Switch to full-frame update if dirty region exceeds threshold
    uint32_t dirtyPixels = region.width * region.height;
    float dirtyRatio = (float)dirtyPixels / (LCD_WIDTH * LCD_HEIGHT);
    if (dirtyRatio > dirtyThreshold) {
        region.x = 0;
        region.y = 0;
        region.width = LCD_WIDTH;
        region.height = LCD_HEIGHT;
    }

    return true;
}

// Main update loop - dirty detection thread
void ILI9486FramebufferV3::updateLoop() {
    std::cout << "[FB_V3] Update loop running" << std::endl;

    // Track which buffer was active last cycle. When it changes, invalidate the incoming
    // buffer's previousFrame so dirty detection fires a full update — this clears the
    // display of whatever the previous app left there.
    uint32_t lastActiveIdx = stats ? stats->activeBufferIndex.load(std::memory_order_relaxed) : 0;

    while (running) {
        // Block until the SPI thread has consumed the previous frame.
        // Replaces the target_fps busy-wait: SPI transfer time is the natural rate limiter,
        // while the previous frame's transfer runs in parallel on spiThread.
        if (frameReady.load()) {
            std::unique_lock<std::mutex> lk(frameMutex);
            frameConsumedCV.wait(lk, [this]{ return !frameReady.load() || !running.load(); });
            lk.unlock();
            if (!running) break;
        }

        auto now = std::chrono::steady_clock::now();

        // Thermal check every 2 seconds — cheap fopen, no need for a separate thread
        auto thermalElapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - lastThermalCheck);
        if (thermalElapsed.count() >= 2) {
            checkThermal();
            lastThermalCheck = now;
        }

        // App SPI speed request — checked every frame cycle (~30 fps).
        // Non-zero = app wants that specific speed; 0 = restore config default.
        // Thermal throttle still applies on top: HOT halves it, CRITICAL quarters it.
        if (stats) {
            uint32_t reqHz = stats->requestedSpiSpeedHz.load(std::memory_order_relaxed);
            if (reqHz != lastReqHz_) {
                lastReqHz_      = reqHz;
                nominalSpiSpeed = (reqHz > 0) ? reqHz : configSpiSpeed_;
                uint32_t ts     = stats->thermalState.load(std::memory_order_relaxed);
                uint32_t hz     = nominalSpiSpeed;
                if (ts >= THERMAL_HOT)      hz = std::max(nominalSpiSpeed / 2, 8000000u);
                if (ts >= THERMAL_CRITICAL) hz = std::max(nominalSpiSpeed / 4, 8000000u);
                lcd.setSPISpeed(hz);
                std::cout << "[FB_V3] SPI speed "
                          << (reqHz > 0 ? "raised to " : "restored to ")
                          << hz / 1000000 << " MHz (app request)" << std::endl;
            }
        }

        auto frameStart = now;
        frameCount++;

        // Check if application is currently drawing
        if (stats && stats->frameInProgress.load(std::memory_order_seq_cst)) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        // Full memory barrier — we must see all writes the app made before it cleared
        // frameInProgress, including any buffer swap it performed.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Re-read activeBufferIndex AFTER the fence so we pick up any swap the app
        // completed before it cleared frameInProgress.
        uint32_t activeIdx = stats ? stats->activeBufferIndex.load(std::memory_order_seq_cst) : 0;

        // If the app switched buffers, invalidate the incoming buffer's previousFrame.
        // This forces a full-frame dirty comparison on the next cycle, which sends a
        // complete refresh to the display — clearing any content left by the previous app.
        if (activeIdx != lastActiveIdx) {
            uint16_t* activePrevious = (activeIdx == 0) ? previousFrame0.data() : previousFrame1.data();
            memset(activePrevious, 0xFF, fbSize);
            lastActiveIdx = activeIdx;
        }

        uint16_t* activeFramebuffer = (activeIdx == 0) ? framebuffer0 : framebuffer1;

        // Snapshot the entire active buffer (~0.075ms). All subsequent detection and packing
        // work from snapshotBuf only — the live SHM is never read again this cycle.
        memcpy(snapshotBuf.data(), activeFramebuffer, fbSize);

        // All detection and packing work from snapshotBuf
        DirtyRegion dirty;
        if (detectDirtyRegion(dirty, snapshotBuf.data(), activeIdx)) {
            std::unique_lock<std::mutex> lock(frameMutex);

            uint16_t* activePrevious = (activeIdx == 0) ? previousFrame0.data() : previousFrame1.data();

            // Write into the buffer the SPI thread is NOT currently reading
            int wi = 1 - pendingBufIdx;

            // Pack dirty rows CONTIGUOUSLY into transferBuf[wi] starting at offset 0.
            // Fast path: when dirty.x==0 and dirty.width==LCD_WIDTH, rows in snapshotBuf
            // are already contiguous — collapse to a single memcpy.
            if (dirty.x == 0 && dirty.width == LCD_WIDTH) {
                int srcOff = dirty.y * LCD_WIDTH;
                size_t bytes = (size_t)dirty.height * LCD_WIDTH * sizeof(uint16_t);
                memcpy(&transferBuf[wi][0],     &snapshotBuf[srcOff], bytes);
                memcpy(&activePrevious[srcOff], &snapshotBuf[srcOff], bytes);
            } else {
                for (int row = 0; row < dirty.height; row++) {
                    int srcOff = (dirty.y + row) * LCD_WIDTH + dirty.x;
                    int dstOff = row * dirty.width;
                    memcpy(&transferBuf[wi][dstOff], &snapshotBuf[srcOff],
                           dirty.width * sizeof(uint16_t));
                    // Keep previousFrame in sync (full-frame layout, for next-cycle comparison)
                    memcpy(&activePrevious[srcOff], &snapshotBuf[srcOff],
                           dirty.width * sizeof(uint16_t));
                }
            }

            pendingBufIdx = wi;
            currentDirty  = dirty;
            frameReady    = true;
            lock.unlock();
            frameCV.notify_one();
        } else {
            // No changes detected — brief pause to avoid spinning on static content
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        // Track detection-cycle latency (not FPS — FPS is counted in spiTransferLoop)
        auto frameEnd  = std::chrono::steady_clock::now();
        auto frameTime = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart);
        recordFrameTime(frameTime.count());
    }
}

// SPI transfer thread - does actual hardware writes
void ILI9486FramebufferV3::spiTransferLoop() {
    std::cout << "[FB_V3] SPI transfer loop running" << std::endl;

    while (running) {
        std::unique_lock<std::mutex> lock(frameMutex);

        // Wait for frame ready
        frameCV.wait(lock, [this] { return frameReady.load() || !running; });

        if (!running) break;
        if (!frameReady) continue;

        // Snapshot what to transfer, release lock BEFORE SPI (restores pipeline parallelism)
        DirtyRegion dirty = currentDirty;
        int ri = pendingBufIdx;  // buffer that was just written by detection
        frameReady = false;
        lock.unlock();

        // Signal updateLoop that it can start detecting the next frame immediately —
        // updateThread fills transferBuf[1-ri] while this thread transfers transferBuf[ri].
        frameConsumedCV.notify_one();

        // SPI transfer runs WITHOUT holding frameMutex — detection fills the other buffer now
        if (dirty.dirty) {
            if (dirty.width == LCD_WIDTH && dirty.height == LCD_HEIGHT) {
                lcd.copyFramebuffer(transferBuf[ri].data(), LCD_WIDTH * LCD_HEIGHT);
            } else {
                lcd.updateRegionFromBuffer(transferBuf[ri].data(), dirty.x, dirty.y, dirty.width, dirty.height);
            }
        }

        if (stats) {
            stats->totalFrames++;
            stats->dirtyPixelCount = dirty.width * dirty.height;
        }

        // FPS counted here — only actual display updates qualify
        updateFpsCounter();
    }
}

void ILI9486FramebufferV3::updateDisplay() {
    // Manual trigger: copy full active framebuffer into the write slot and signal SPI
    std::unique_lock<std::mutex> lock(frameMutex);

    uint32_t activeIdx = stats ? stats->activeBufferIndex.load() : 0;
    uint16_t* activeFramebuffer = (activeIdx == 0) ? framebuffer0 : framebuffer1;

    int wi = 1 - pendingBufIdx;
    memcpy(transferBuf[wi].data(), activeFramebuffer, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
    pendingBufIdx = wi;

    currentDirty = {0, 0, LCD_WIDTH, LCD_HEIGHT, true};
    frameReady = true;
    lock.unlock();
    frameCV.notify_one();
}

void ILI9486FramebufferV3::enableAutoDirtyDetection(bool enable) {
    autoDirtyDetection = enable;
    std::cout << "[FB_V3] Auto dirty detection: " << (enable ? "ON" : "OFF") << std::endl;
}

void ILI9486FramebufferV3::enableFrameSkipping(bool enable) {
    frameSkippingEnabled = enable;
    std::cout << "[FB_V3] Frame skipping: " << (enable ? "ON" : "OFF") << std::endl;
}

void ILI9486FramebufferV3::setDirtyThreshold(float threshold) {
    dirtyThreshold = threshold;
    std::cout << "[FB_V3] Dirty threshold set to " << (threshold * 100) << "%" << std::endl;
}

void ILI9486FramebufferV3::updateFpsCounter() {
    frameCountForFps++;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - fpsCounterStart);

    if (elapsed.count() >= 1) {
        if (stats) {
            stats->currentFps = frameCountForFps;
        }
        frameCountForFps = 0;
        fpsCounterStart = now;
    }
}

void ILI9486FramebufferV3::recordFrameTime(uint32_t microseconds) {
    if (stats) {
        // Moving average
        uint32_t current = stats->avgUpdateTimeUs.load();
        stats->avgUpdateTimeUs = (current * 9 + microseconds) / 10;
    }
}

void ILI9486FramebufferV3::resetStats() {
    if (stats) {
        stats->totalFrames = 0;
        stats->droppedFrames = 0;
        stats->currentFps = 0;
        stats->dirtyPixelCount = 0;
        stats->avgUpdateTimeUs = 0;
        stats->frameInProgress = false;  // Reset frame lock
    }
}

void ILI9486FramebufferV3::setOrientation(int degrees) {
    lcd.setOrientation(degrees);
}

void ILI9486FramebufferV3::setMirrorX(bool mirror) {
    lcd.setMirrorX(mirror);
}

void ILI9486FramebufferV3::setMirrorY(bool mirror) {
    lcd.setMirrorY(mirror);
}

// Double buffering methods
uint16_t* ILI9486FramebufferV3::getBackBuffer() {
    // Back buffer is the NON-active buffer
    uint32_t activeIdx = stats ? stats->activeBufferIndex.load() : 0;
    return (activeIdx == 0) ? framebuffer1 : framebuffer0;
}

uint16_t* ILI9486FramebufferV3::getFrontBuffer() {
    // Front buffer is the active buffer (currently displaying)
    uint32_t activeIdx = stats ? stats->activeBufferIndex.load() : 0;
    return (activeIdx == 0) ? framebuffer0 : framebuffer1;
}

void ILI9486FramebufferV3::swapBuffers() {
    if (!stats) return;
    
    // Atomically swap buffer index (0 <-> 1)
    uint32_t current = stats->activeBufferIndex.load();
    stats->activeBufferIndex.store(1 - current);
    
    // Driver will pick up the new active buffer on next frame detection
}

// C API Implementation
extern "C" {
    void* fb_v3_create(const char* spiDevice, int dcPin, int rstPin) {
        return new ILI9486FramebufferV3(spiDevice, dcPin, rstPin);
    }

    int fb_v3_init(void* fb) {
        return ((ILI9486FramebufferV3*)fb)->init() ? 1 : 0;
    }

    void fb_v3_start(void* fb) {
        ((ILI9486FramebufferV3*)fb)->startUpdateLoop();
    }

    void fb_v3_stop(void* fb) {
        ((ILI9486FramebufferV3*)fb)->stopUpdateLoop();
    }

    void fb_v3_destroy(void* fb) {
        delete (ILI9486FramebufferV3*)fb;
    }

    uint16_t* fb_v3_get_buffer(void* fb) {
        return ((ILI9486FramebufferV3*)fb)->getFramebuffer();
    }

    void fb_v3_update(void* fb) {
        ((ILI9486FramebufferV3*)fb)->updateDisplay();
    }

    uint16_t* fb_v3_map_shared_memory() {
        int fd = shm_open(FB_V3_SHM_NAME, O_RDWR, 0666);
        if (fd < 0) return nullptr;

        void* addr = mmap(nullptr, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        return (addr == MAP_FAILED) ? nullptr : (uint16_t*)addr;
    }

    void fb_v3_unmap_shared_memory(uint16_t* addr) {
        if (addr) munmap(addr, FB_SIZE);
    }

    FramebufferStats* fb_v3_get_stats() {
        int fd = shm_open(FB_V3_STATS_SHM_NAME, O_RDONLY, 0666);
        if (fd < 0) return nullptr;

        void* addr = mmap(nullptr, sizeof(FramebufferStats), PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        return (addr == MAP_FAILED) ? nullptr : (FramebufferStats*)addr;
    }
    
    // Double buffer API
    uint16_t* fb_v3_get_back_buffer() {
        // Map stats first to get activeBufferIndex
        FramebufferStats* stats = fb_v3_get_stats();
        if (!stats) return nullptr;
        
        uint32_t activeIdx = stats->activeBufferIndex.load();
        munmap(stats, sizeof(FramebufferStats));
        
        // Back buffer is the NON-active buffer
        const char* shmName = (activeIdx == 0) ? FB_V3_SHM_NAME_1 : FB_V3_SHM_NAME_0;
        
        int fd = shm_open(shmName, O_RDWR, 0666);
        if (fd < 0) return nullptr;

        void* addr = mmap(nullptr, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        return (addr == MAP_FAILED) ? nullptr : (uint16_t*)addr;
    }
    
    uint16_t* fb_v3_get_front_buffer() {
        // Map stats first to get activeBufferIndex
        FramebufferStats* stats = fb_v3_get_stats();
        if (!stats) return nullptr;
        
        uint32_t activeIdx = stats->activeBufferIndex.load();
        munmap(stats, sizeof(FramebufferStats));
        
        // Front buffer is the active buffer
        const char* shmName = (activeIdx == 0) ? FB_V3_SHM_NAME_0 : FB_V3_SHM_NAME_1;
        
        int fd = shm_open(shmName, O_RDWR, 0666);
        if (fd < 0) return nullptr;

        void* addr = mmap(nullptr, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);

        return (addr == MAP_FAILED) ? nullptr : (uint16_t*)addr;
    }
    
    void fb_v3_swap_buffers() {
        // Map stats to swap activeBufferIndex
        FramebufferStats* stats = fb_v3_get_stats();
        if (!stats) return;
        
        uint32_t current = stats->activeBufferIndex.load();
        stats->activeBufferIndex.store(1 - current);
        
        munmap(stats, sizeof(FramebufferStats));
    }
}
