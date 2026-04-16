#include "fb_ili9486_v3.h"
#include "config.h"
#include <iostream>
#include <cstdio>
#include <signal.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <iomanip>
#include <unistd.h>
#include <libgen.h>
#include <linux/limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

std::atomic<bool> running(true);

static const char* PID_FILE = "/tmp/fb_main_v3.pid";

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

void signalHandler(int signum) {
    std::cout << "\n[FB_V3_MAIN] Received signal " << signum << ", shutting down..." << std::endl;
    running = false;
    unlink(PID_FILE);
}

void printStats(FramebufferStats* stats) {
    if (!stats) return;

    static const char* thermalNames[] = { "OK", "WARM", "HOT", "CRIT" };
    uint32_t tstate = stats->thermalState.load();
    uint32_t tempC  = stats->cpuTempMilliC.load() / 1000;
    const char* tname = (tstate < 4) ? thermalNames[tstate] : "??";

    std::cout << "\r[FB_V3] FPS: " << std::setw(2) << stats->currentFps.load()
              << " | Frames: " << std::setw(6) << stats->totalFrames.load()
              << " | Dirty: " << std::setw(6) << stats->dirtyPixelCount.load() << "px"
              << " | Avg: " << std::setw(5) << stats->avgUpdateTimeUs.load() << "us"
              << " | CPU: " << std::setw(2) << tempC << "°C [" << tname << "]"
              << std::flush;
}

int main(int argc, char** argv) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << R"(
╔═══════════════════════════════════════════════╗
║   ILI9486 Framebuffer Driver V3 - TURBO      ║
║   100x Better Performance Edition             ║
╚═══════════════════════════════════════════════╝
)" << std::endl;

    std::cout << "[FB_V3_MAIN] Starting ILI9486 Framebuffer Driver V3..." << std::endl;

    // Load configuration file
    Config config;
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len != -1) {
        exePath[len] = '\0';
        std::string configPath = std::string(dirname(exePath)) + "/settings.conf";
        std::cout << "[FB_V3_MAIN] Looking for config at: " << configPath << std::endl;
        if (!config.load(configPath)) {
            std::cout << "[FB_V3_MAIN] Using default settings (no config file found)" << std::endl;
        } else {
            std::cout << "[FB_V3_MAIN] Configuration loaded successfully" << std::endl;
        }
    }

    // Load settings from config
    std::string spiDevice = config.getString("spi_device", "/dev/spidev0.0");
    int gpioDC = config.getInt("gpio_dc", 24);
    int gpioRST = config.getInt("gpio_rst", 25);
    uint32_t spiSpeed = config.getUInt("spi_speed", 32000000);
    int orientation = config.getInt("orientation", 0);
    bool mirrorX = config.getBool("mirror_x", false);
    bool mirrorY = config.getBool("mirror_y", false);
    bool enableDirtyDetection = config.getBool("dirty_detection", true);
    bool enableFrameSkipping = config.getBool("frame_skipping", true);
    float dirtyThreshold = config.getFloat("dirty_threshold", 0.5f);
    bool showStats = config.getBool("show_stats", true);
    int statsInterval = config.getInt("stats_interval", 500);
    bool verbose = config.getBool("verbose", false);

    // Debug: Show loaded values
    if (verbose) {
        std::cout << "[FB_V3_MAIN] Loaded settings from config:" << std::endl;
        std::cout << "  spi_device: " << spiDevice << std::endl;
        std::cout << "  gpio_dc: " << gpioDC << std::endl;
        std::cout << "  gpio_rst: " << gpioRST << std::endl;
        std::cout << "  spi_speed: " << spiSpeed << " Hz (" << spiSpeed/1000000 << " MHz)" << std::endl;
        std::cout << "  orientation: " << orientation << " degrees" << std::endl;
        std::cout << "  mirror_x: " << mirrorX << std::endl;
        std::cout << "  mirror_y: " << mirrorY << std::endl;
        std::cout << "  dirty_detection: " << enableDirtyDetection << std::endl;
        std::cout << "  frame_skipping: " << enableFrameSkipping << std::endl;
        std::cout << "  dirty_threshold: " << dirtyThreshold << std::endl;
        std::cout << "  show_stats: " << showStats << std::endl;
        std::cout << "  stats_interval: " << statsInterval << std::endl;
        std::cout << "  verbose: " << verbose << std::endl;
    }

    // Command line overrides
    bool daemonMode = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--no-dirty") {
            enableDirtyDetection = false;
        } else if (arg == "--no-skip") {
            enableFrameSkipping = false;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--daemon" || arg == "-d") {
            daemonMode = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --daemon, -d     Run as background daemon (no terminal output)" << std::endl;
            std::cout << "  --no-dirty       Disable automatic dirty detection" << std::endl;
            std::cout << "  --no-skip        Disable frame skipping" << std::endl;
            std::cout << "  --verbose, -v    Enable verbose logging" << std::endl;
            std::cout << "  --help, -h       Show this help" << std::endl;
            std::cout << std::endl;
            std::cout << "Daemon Mode:" << std::endl;
            std::cout << "  Use --daemon to run in background without blocking terminal" << std::endl;
            std::cout << "  Perfect for running alongside touch driver and test apps" << std::endl;
            std::cout << "  Example: sudo ./Drivers/fb_main_v3 --daemon" << std::endl;
            std::cout << std::endl;
            std::cout << "Configuration file: Drivers/settings.conf" << std::endl;
            return 0;
        }
    }
    
    // Single-instance guard — check before fork so both daemon and non-daemon paths are covered
    {
        pid_t existing = checkExistingInstance();
        if (existing) {
            std::cerr << "[FB_V3_MAIN] Already running (PID " << existing << "). Kill it first:  sudo kill " << existing << std::endl;
            return 1;
        }
    }

    // Fork to background if daemon mode
    if (daemonMode) {
        std::cout << "[FB_V3_MAIN] Starting in DAEMON mode..." << std::endl;

        pid_t pid = fork();
        if (pid < 0) {
            std::cerr << "[FB_V3_MAIN] Failed to fork daemon process" << std::endl;
            return 1;
        }
        
        if (pid > 0) {
            // Parent process - exit and let child run
            std::cout << "[FB_V3_MAIN] Daemon started with PID: " << pid << std::endl;
            std::cout << "[FB_V3_MAIN] Framebuffer driver running in background" << std::endl;
            std::cout << "[FB_V3_MAIN] To stop: sudo kill " << pid << std::endl;
            return 0;
        }
        
        // Child process - continue as daemon
        // Create new session
        if (setsid() < 0) {
            return 1;
        }

        // Write child's PID before redirecting stdout/stderr to /dev/null
        writePidFile();

        // Redirect stdout/stderr to /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        
        // Disable stats in daemon mode
        showStats = false;
    } else {
        // Non-daemon: write PID now (daemon child already wrote it above)
        writePidFile();
    }

    std::cout << "[FB_V3_MAIN] Configuration:" << std::endl;
    std::cout << "  SPI Device: " << spiDevice << std::endl;
    std::cout << "  SPI Speed: " << spiSpeed / 1000000 << " MHz" << std::endl;
    std::cout << "  GPIO DC: " << gpioDC << ", RST: " << gpioRST << std::endl;
    std::cout << "  Orientation: " << orientation << " degrees" << std::endl;
    std::cout << "  Mirror X: " << (mirrorX ? "ON" : "OFF") << std::endl;
    std::cout << "  Mirror Y: " << (mirrorY ? "ON" : "OFF") << std::endl;
    std::cout << "  Dirty Detection: " << (enableDirtyDetection ? "ON" : "OFF") << std::endl;
    std::cout << "  Frame Skipping: " << (enableFrameSkipping ? "ON" : "OFF") << std::endl;
    std::cout << "  Dirty Threshold: " << (dirtyThreshold * 100) << "%" << std::endl;
    std::cout << std::endl;

    std::cout << "[FB_V3_MAIN] Features enabled:" << std::endl;
    std::cout << "  ✓ NEON SIMD dirty detection" << std::endl;
    std::cout << "  ✓ Dual-threaded pipeline (detection + SPI)" << std::endl;
    std::cout << "  ✓ Zero-copy shared memory" << std::endl;
    std::cout << "  ✓ Optimized dirty-region memcpy" << std::endl;
    std::cout << "  ✓ Fixed color format (BGR565)" << std::endl;
    std::cout << "  ✓ Configurable SPI speed (settings.conf)" << std::endl;
    std::cout << std::endl;

    // Lock all current and future pages into RAM — prevents kernel from paging out
    // the driver's working set (SPI buffers, framebuffer) during transfers.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::cerr << "[FB_V3_MAIN] Warning: mlockall failed (non-fatal)" << std::endl;
    }

    ILI9486FramebufferV3 fb(spiDevice, gpioDC, gpioRST, spiSpeed);

    if (!fb.init()) {
        std::cerr << "[FB_V3_MAIN] Failed to initialize framebuffer!" << std::endl;
        return 1;
    }

    // Configure display settings
    fb.setOrientation(orientation);
    fb.setMirrorX(mirrorX);
    fb.setMirrorY(mirrorY);

    // Configure advanced features
    fb.enableAutoDirtyDetection(enableDirtyDetection);
    fb.enableFrameSkipping(enableFrameSkipping);
    fb.setDirtyThreshold(dirtyThreshold);

    std::cout << "[FB_V3_MAIN] Framebuffer initialized successfully" << std::endl;
    std::cout << "[FB_V3_MAIN] Shared memory: " << FB_V3_SHM_NAME << std::endl;
    std::cout << "[FB_V3_MAIN] Stats memory: " << FB_V3_STATS_SHM_NAME << std::endl;
    std::cout << "[FB_V3_MAIN] Applications can now connect..." << std::endl;
    std::cout << std::endl;

    fb.startUpdateLoop();

    std::cout << "[FB_V3_MAIN] Driver running. Press Ctrl+C to stop." << std::endl;
    if (showStats) {
        std::cout << "[FB_V3_MAIN] Live stats:" << std::endl;
    }

    // Main loop - display stats
    auto lastStatsUpdate = std::chrono::steady_clock::now();
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (showStats) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatsUpdate);

            if (elapsed.count() >= statsInterval) {
                printStats(fb.getStats());
                lastStatsUpdate = now;
            }
        }
    }

    std::cout << std::endl;
    std::cout << "[FB_V3_MAIN] Stopping driver..." << std::endl;
    fb.stopUpdateLoop();

    // Final stats
    std::cout << "\n[FB_V3_MAIN] Final Statistics:" << std::endl;
    FramebufferStats* stats = fb.getStats();
    if (stats) {
        std::cout << "  Total Frames: " << stats->totalFrames.load() << std::endl;
        std::cout << "  Dropped Frames: " << stats->droppedFrames.load() << std::endl;
        std::cout << "  Average Update Time: " << stats->avgUpdateTimeUs.load() << " µs" << std::endl;
    }

    std::cout << "[FB_V3_MAIN] Driver stopped cleanly." << std::endl;
    unlink(PID_FILE);
    return 0;
}
