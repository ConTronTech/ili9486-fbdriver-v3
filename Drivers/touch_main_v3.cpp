#include "touch_xpt2046_v3.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <csignal>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

#define TOUCH_SHM_NAME "/xpt2046_touch_v3"
#define TOUCH_STATS_SHM_NAME "/xpt2046_touch_v3_stats"
#define EVENT_QUEUE_SIZE 64

static volatile bool running = true;

void signalHandler(int signum) {
    std::cout << "\n[TOUCH] Received signal " << signum << ", shutting down..." << std::endl;
    running = false;
}

// Parse configuration file
bool parseConfig(const char* configPath, TouchConfig& config) {
    // Set defaults FIRST (so we always have valid values)
    config.spiDevice = "/dev/spidev0.1";
    config.spiSpeed = 2000000;
    config.orientation = 0;
    config.nativeWidth = 320;       // XPT2046 is portrait in native form
    config.nativeHeight = 480;
    config.displayWidth = 480;      // Landscape display for orientation=0
    config.displayHeight = 320;
    config.calXMin = 200;
    config.calXMax = 3900;
    config.calYMin = 200;
    config.calYMax = 3900;
    config.pressureThreshold = 1500;
    config.pollRateHz = 100;
    
    // Try to open config file
    std::ifstream file(configPath);
    if (!file.is_open()) {
        std::cerr << "[TOUCH] Warning: Could not open config file: " << configPath << std::endl;
        std::cerr << "[TOUCH] Using default configuration" << std::endl;
        return false;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        std::istringstream iss(line);
        std::string key;
        char equals;
        
        if (iss >> key >> equals && equals == '=') {
            if (key == "touch_spi_device") {
                std::string value;
                iss >> value;
                config.spiDevice = value;
            } else if (key == "touch_spi_speed") {
                iss >> config.spiSpeed;
            } else if (key == "orientation") {
                iss >> config.orientation;
            } else if (key == "touch_native_width") {
                iss >> config.nativeWidth;
            } else if (key == "touch_native_height") {
                iss >> config.nativeHeight;
            } else if (key == "touch_display_width") {
                iss >> config.displayWidth;
            } else if (key == "touch_display_height") {
                iss >> config.displayHeight;
            } else if (key == "touch_cal_x_min") {
                iss >> config.calXMin;
            } else if (key == "touch_cal_x_max") {
                iss >> config.calXMax;
            } else if (key == "touch_cal_y_min") {
                iss >> config.calYMin;
            } else if (key == "touch_cal_y_max") {
                iss >> config.calYMax;
            } else if (key == "touch_pressure_threshold") {
                iss >> config.pressureThreshold;
            } else if (key == "touch_poll_rate") {
                iss >> config.pollRateHz;
            }
        }
    }
    
    return true;
}

int main(int argc, char** argv) {
    // Install signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    std::cout << R"(
╔═══════════════════════════════════════════════╗
║   XPT2046 Touch Driver V3                     ║
║   Resistive Touch Controller for RPi Display  ║
╚═══════════════════════════════════════════════╝
)" << std::endl;
    
    // Parse configuration
    TouchConfig config;
    const char* configPath = "settings.conf";
    if (argc > 1) {
        configPath = argv[1];
    }
    
    // Try current directory first, then Drivers directory
    if (!parseConfig(configPath, config)) {
        // Try Drivers subdirectory
        if (!parseConfig("Drivers/settings.conf", config)) {
            // Defaults already set in parseConfig
        }
    }
    
    std::cout << "[TOUCH] Configuration:" << std::endl;
    std::cout << "  SPI Device: " << config.spiDevice << std::endl;
    std::cout << "  SPI Speed: " << config.spiSpeed << " Hz" << std::endl;
    std::cout << "  Orientation: " << config.orientation << "°" << std::endl;
    std::cout << "  Native Sensor: " << config.nativeWidth << "x" << config.nativeHeight << std::endl;
    std::cout << "  Display: " << config.displayWidth << "x" << config.displayHeight << std::endl;
    std::cout << "  Calibration: X[" << config.calXMin << "-" << config.calXMax << "] "
              << "Y[" << config.calYMin << "-" << config.calYMax << "]" << std::endl;
    std::cout << "  Pressure Threshold: " << config.pressureThreshold << std::endl;
    std::cout << "  Poll Rate: " << config.pollRateHz << " Hz" << std::endl;
    
    // Initialize touch controller
    TouchXPT2046 touch(config);
    if (!touch.init()) {
        std::cerr << "[TOUCH] Failed to initialize touch controller" << std::endl;
        return 1;
    }
    
    // Create shared memory for event queue
    shm_unlink(TOUCH_SHM_NAME);  // Clean up any previous instance
    int shmFd = shm_open(TOUCH_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shmFd < 0) {
        std::cerr << "[TOUCH] Failed to create event queue shared memory" << std::endl;
        return 1;
    }
    
    size_t shmSize = sizeof(TouchEvent) * EVENT_QUEUE_SIZE;
    if (ftruncate(shmFd, shmSize) < 0) {
        std::cerr << "[TOUCH] Failed to set event queue size" << std::endl;
        close(shmFd);
        return 1;
    }
    
    TouchEvent* eventQueue = (TouchEvent*)mmap(nullptr, shmSize,
                                                PROT_READ | PROT_WRITE,
                                                MAP_SHARED, shmFd, 0);
    close(shmFd);
    
    if (eventQueue == MAP_FAILED) {
        std::cerr << "[TOUCH] Failed to mmap event queue" << std::endl;
        return 1;
    }
    
    // Initialize event queue
    memset(eventQueue, 0, shmSize);
    
    // Create shared memory for stats
    shm_unlink(TOUCH_STATS_SHM_NAME);
    int statsFd = shm_open(TOUCH_STATS_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (statsFd < 0) {
        std::cerr << "[TOUCH] Failed to create stats shared memory" << std::endl;
        munmap(eventQueue, shmSize);
        return 1;
    }
    
    if (ftruncate(statsFd, sizeof(TouchStats)) < 0) {
        std::cerr << "[TOUCH] Failed to set stats size" << std::endl;
        close(statsFd);
        munmap(eventQueue, shmSize);
        return 1;
    }
    
    TouchStats* stats = (TouchStats*)mmap(nullptr, sizeof(TouchStats),
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, statsFd, 0);
    close(statsFd);
    
    if (stats == MAP_FAILED) {
        std::cerr << "[TOUCH] Failed to mmap stats" << std::endl;
        munmap(eventQueue, shmSize);
        return 1;
    }
    
    // Initialize stats
    stats->totalEvents.store(0);
    stats->currentPressure.store(0);
    stats->touchActive.store(false);
    stats->pollRate.store(config.pollRateHz);
    stats->driverActive.store(true);
    stats->eventQueueHead.store(0);
    stats->eventQueueTail.store(0);
    
    std::cout << "[TOUCH] Shared memory initialized" << std::endl;
    std::cout << "[TOUCH] Event queue: " << TOUCH_SHM_NAME << " (" << EVENT_QUEUE_SIZE << " events)" << std::endl;
    std::cout << "[TOUCH] Stats: " << TOUCH_STATS_SHM_NAME << std::endl;
    std::cout << "[TOUCH] Touch driver running... Press Ctrl+C to stop" << std::endl;
    
    // Main polling loop
    auto pollInterval = std::chrono::microseconds(1000000 / config.pollRateHz);
    uint64_t eventCount = 0;
    
    while (running) {
        auto loopStart = std::chrono::steady_clock::now();
        
        // Read touch event
        TouchEvent event;
        if (touch.readTouch(event)) {
            // Valid event - add to queue
            uint32_t tail = stats->eventQueueTail.load();
            uint32_t nextTail = (tail + 1) % EVENT_QUEUE_SIZE;
            
            // Check if queue is full
            if (nextTail != stats->eventQueueHead.load()) {
                eventQueue[tail] = event;
                stats->eventQueueTail.store(nextTail);
                
                stats->totalEvents.fetch_add(1);
                stats->currentPressure.store(event.pressure);
                stats->touchActive.store(event.state != TOUCH_RELEASED);
                
                eventCount++;
                
                // Log PRESS and RELEASE events immediately, HELD events periodically
                bool shouldLog = (event.state == TOUCH_PRESSED || event.state == TOUCH_RELEASED) ||
                                (eventCount % 10 == 0);
                
                if (shouldLog) {
                    std::cout << "[TOUCH] Event #" << eventCount 
                              << " - State: " << (event.state == TOUCH_PRESSED ? "PRESS" : 
                                                 event.state == TOUCH_HELD ? "HELD" : "RELEASE")
                              << " Position: (" << event.x << "," << event.y << ")"
                              << " Pressure: " << event.pressure << std::endl;
                }
            }
        }
        
        // Sleep for poll interval
        auto loopEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(loopEnd - loopStart);
        if (elapsed < pollInterval) {
            std::this_thread::sleep_for(pollInterval - elapsed);
        }
    }
    
    // Cleanup
    std::cout << "[TOUCH] Cleaning up..." << std::endl;
    stats->driverActive.store(false);
    
    munmap(eventQueue, shmSize);
    munmap(stats, sizeof(TouchStats));
    
    shm_unlink(TOUCH_SHM_NAME);
    shm_unlink(TOUCH_STATS_SHM_NAME);
    
    std::cout << "[TOUCH] Driver stopped. Total events: " << eventCount << std::endl;
    
    return 0;
}
