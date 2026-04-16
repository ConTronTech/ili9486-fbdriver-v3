/**
 * Touch Debugger V3 - Terminal Output Only
 *
 * Simple diagnostic tool for debugging touch controller issues.
 * Displays real-time touch coordinates, pressure, and state in terminal.
 * Shows both transformed (display) coordinates AND raw ADC values.
 *
 * Features:
 * - Terminal-only output (no display driver required)
 * - Real-time coordinate display
 * - Raw ADC value display for calibration debugging
 * - Touch state tracking (PRESSED/HELD/RELEASED)
 * - Pressure/Z value monitoring
 *
 * Usage: sudo ./touch_debugger_v3
 *
 * Requirements:
 * - Root access (for SPI hardware)
 * - XPT2046 on /dev/spidev0.1
 * - No display driver needed!
 */

#include <iostream>
#include <iomanip>
#include <csignal>
#include <unistd.h>
#include "../../Drivers/touch_xpt2046_v3.h"
#include "../../Drivers/config.h"

static volatile bool running = true;

void signalHandler(int) {
    running = false;
}

/**
 * Convert TouchState enum to readable string
 */
const char* stateToString(TouchState state) {
    switch (state) {
        case TOUCH_RELEASED: return "RELEASED";
        case TOUCH_PRESSED:  return "PRESSED ";
        case TOUCH_HELD:     return "HELD    ";
        default:             return "UNKNOWN ";
    }
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << R"(
╔══════════════════════════════════════════════════════╗
║   Touch Debugger V3 - Terminal Output                ║
║   Real-time touch coordinate display                 ║
╚══════════════════════════════════════════════════════╝
)" << std::endl;

    std::cout << "[DEBUG] Initializing touch controller..." << std::endl;

    // Load configuration (try multiple paths)
    Config config;
    const char* configPaths[] = {
        "../../Drivers/settings.conf",  // From Apps/Touch/
        "./Drivers/settings.conf",      // From project root
        "Drivers/settings.conf"         // From project root (alternate)
    };

    bool configLoaded = false;
    for (const char* path : configPaths) {
        if (config.load(path)) {
            std::cout << "[DEBUG] Loaded config from: " << path << std::endl;
            configLoaded = true;
            break;
        }
    }

    if (!configLoaded) {
        std::cerr << "[DEBUG] WARNING: Could not find settings.conf, using defaults" << std::endl;
    }

    // Setup touch controller
    TouchConfig touchCfg;
    touchCfg.spiDevice = config.getString("touch_spi_device", "/dev/spidev0.1");
    touchCfg.spiSpeed = config.getUInt("touch_spi_speed", 2000000);
    touchCfg.orientation = config.getInt("orientation", 0);
    touchCfg.nativeWidth = config.getInt("touch_native_width", 320);
    touchCfg.nativeHeight = config.getInt("touch_native_height", 480);
    touchCfg.displayWidth = config.getInt("touch_display_width", 480);
    touchCfg.displayHeight = config.getInt("touch_display_height", 320);
    // Load legacy calibration (for fallback)
    touchCfg.calXMin = config.getInt("touch_cal_x_min", 200);
    touchCfg.calXMax = config.getInt("touch_cal_x_max", 3900);
    touchCfg.calYMin = config.getInt("touch_cal_y_min", 200);
    touchCfg.calYMax = config.getInt("touch_cal_y_max", 3900);

    // Load 9-point calibration if available
    touchCfg.numCalPoints = config.getInt("touch_cal_points", 0);
    if (touchCfg.numCalPoints == 9) {
        for (int i = 0; i < 9; i++) {
            std::string nativeXKey = "touch_cal_point_" + std::to_string(i) + "_native_x";
            std::string nativeYKey = "touch_cal_point_" + std::to_string(i) + "_native_y";
            std::string rawXKey = "touch_cal_point_" + std::to_string(i) + "_raw_x";
            std::string rawYKey = "touch_cal_point_" + std::to_string(i) + "_raw_y";

            touchCfg.calPoints[i].nativeX = config.getInt(nativeXKey, 0);
            touchCfg.calPoints[i].nativeY = config.getInt(nativeYKey, 0);
            touchCfg.calPoints[i].rawX = config.getInt(rawXKey, 0);
            touchCfg.calPoints[i].rawY = config.getInt(rawYKey, 0);
        }
    }

    touchCfg.pressureThreshold = config.getUInt("touch_pressure_threshold", 1200);
    touchCfg.pollRateHz = config.getInt("touch_poll_rate", 100);

    std::cout << "[DEBUG] Configuration loaded:" << std::endl;
    std::cout << "[DEBUG]   Orientation: " << touchCfg.orientation << "°" << std::endl;
    std::cout << "[DEBUG]   Display: " << touchCfg.displayWidth << "x" << touchCfg.displayHeight << std::endl;
    if (touchCfg.numCalPoints == 9) {
        std::cout << "[DEBUG]   Calibration: 9-point bilinear interpolation" << std::endl;
    } else {
        std::cout << "[DEBUG]   Calibration: Legacy 4-point linear" << std::endl;
        std::cout << "[DEBUG]   Calibration X: " << touchCfg.calXMin << " - " << touchCfg.calXMax << std::endl;
        std::cout << "[DEBUG]   Calibration Y: " << touchCfg.calYMin << " - " << touchCfg.calYMax << std::endl;
    }
    std::cout << "[DEBUG]   Pressure threshold: " << touchCfg.pressureThreshold << std::endl;

    // Initialize touch controller
    TouchXPT2046 touch(touchCfg);

    if (!touch.init()) {
        std::cerr << "\n[DEBUG] ERROR: Failed to initialize touch controller!" << std::endl;
        std::cerr << "[DEBUG] Possible causes:" << std::endl;
        std::cerr << "[DEBUG]   1. Not running with sudo" << std::endl;
        std::cerr << "[DEBUG]   2. XPT2046 not connected to " << touchCfg.spiDevice << std::endl;
        std::cerr << "[DEBUG]   3. SPI not enabled (run: sudo raspi-config)" << std::endl;
        return 1;
    }

    std::cout << "[DEBUG] Touch controller initialized successfully" << std::endl;

    std::cout << "\n[DEBUG] ════════════════════════════════════════════" << std::endl;
    std::cout << "[DEBUG] Touch debugger running..." << std::endl;
    std::cout << "[DEBUG] Touch the screen to see coordinates" << std::endl;
    std::cout << "[DEBUG] Press Ctrl+C to exit" << std::endl;
    std::cout << "[DEBUG] ════════════════════════════════════════════\n" << std::endl;

    std::cout << "Format: [STATE] Display(X, Y) Pressure=XXXX | Raw ADC(X, Y, Z)" << std::endl;
    std::cout << "────────────────────────────────────────────────────────────────\n" << std::endl;

    TouchState lastState = TOUCH_RELEASED;
    uint64_t touchCount = 0;

    // Main debug loop
    while (running) {
        TouchEvent event;
        if (touch.readTouch(event)) {
            // Get raw ADC values for debugging
            int rawX, rawY, rawZ;
            touch.getRawPosition(rawX, rawY, rawZ);

            // Only print when touch state changes or when actively touching
            if (event.state != TOUCH_RELEASED || lastState != TOUCH_RELEASED) {
                // Print formatted output
                std::cout << "[" << stateToString(event.state) << "] "
                          << "Display(" << std::setw(3) << event.x
                          << ", " << std::setw(3) << event.y << ") "
                          << "Pressure=" << std::setw(4) << event.pressure << " | "
                          << "Raw ADC(" << std::setw(4) << rawX
                          << ", " << std::setw(4) << rawY
                          << ", " << std::setw(4) << rawZ << ")";

                // Add event counter for PRESSED events
                if (event.state == TOUCH_PRESSED) {
                    touchCount++;
                    std::cout << "  [Touch #" << touchCount << "]";
                }

                std::cout << std::endl;
            }

            lastState = event.state;
        }

        usleep(10000);  // 10ms polling (100Hz)
    }

    std::cout << "\n[DEBUG] ════════════════════════════════════════════" << std::endl;
    std::cout << "[DEBUG] Touch debugger stopped" << std::endl;
    std::cout << "[DEBUG] Total touches recorded: " << touchCount << std::endl;
    std::cout << "[DEBUG] ════════════════════════════════════════════\n" << std::endl;

    return 0;
}
