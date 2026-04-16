/**
 * Touch Calibration Visual V3 - Direct Hardware Access
 *
 * Calibrates XPT2046 touch controller with visual on-screen feedback.
 * Uses Direct Hardware Access pattern - NO DAEMON REQUIRED!
 *
 * Features:
 * - Initializes both ILI9486 display AND XPT2046 touch directly
 * - 4-corner + center calibration for accuracy
 * - Real-time visual feedback (cyan dot shows detected touch position)
 * - Shows raw ADC values and transformed coordinates in real-time
 * - Orientation-aware calibration (properly handles 0/90/180/270°)
 * - Post-calibration validation test (9-point accuracy check)
 * - Automatically saves calibration to Drivers/settings.conf
 * - Proper double-buffering with buffer remapping
 *
 * Calibration Process:
 * 1. Touch 5 calibration points (4 corners + center)
 * 2. System calculates optimal calibration values
 * 3. Automatic 9-point validation test
 * 4. Visual feedback: Green = Excellent, Blue = Good, Yellow = Needs recalibration
 *
 * Usage: sudo ./touch_calibrate_visual_v3
 *
 * Requirements:
 * - Root access (for SPI hardware)
 * - ILI9486 on /dev/spidev0.0
 * - XPT2046 on /dev/spidev0.1
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <csignal>
#include <unistd.h>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include "../../Drivers/ili9486.h"
#include "../../Drivers/touch_xpt2046_v3.h"
#include "../../Drivers/config.h"

// Display dimensions (framebuffer is always 320x480 portrait)
#define FB_WIDTH 320
#define FB_HEIGHT 480

// Colors (RGB565)
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF

static volatile bool running = true;

void signalHandler(int) {
    running = false;
}

/**
 * Draw a crosshair at the given position
 * @param fb Framebuffer pointer
 * @param cx Center X coordinate
 * @param cy Center Y coordinate
 * @param color Crosshair color
 * @param size Crosshair arm length
 */
void drawCrosshair(uint16_t* fb, int cx, int cy, uint16_t color, int size = 20) {
    // Horizontal line
    for (int x = cx - size; x <= cx + size; x++) {
        if (x >= 0 && x < FB_WIDTH && cy >= 0 && cy < FB_HEIGHT) {
            fb[cy * FB_WIDTH + x] = color;
        }
    }
    // Vertical line
    for (int y = cy - size; y <= cy + size; y++) {
        if (cx >= 0 && cx < FB_WIDTH && y >= 0 && y < FB_HEIGHT) {
            fb[y * FB_WIDTH + cx] = color;
        }
    }
    // Center circle (3px radius)
    for (int dy = -3; dy <= 3; dy++) {
        for (int dx = -3; dx <= 3; dx++) {
            if (dx*dx + dy*dy <= 9) {
                int px = cx + dx;
                int py = cy + dy;
                if (px >= 0 && px < FB_WIDTH && py >= 0 && py < FB_HEIGHT) {
                    fb[py * FB_WIDTH + px] = color;
                }
            }
        }
    }
}

/**
 * Draw a circle outline
 */
void drawCircle(uint16_t* fb, int cx, int cy, int radius, uint16_t color) {
    for (int angle = 0; angle < 360; angle += 3) {
        int x = cx + radius * cos(angle * 3.14159 / 180.0);
        int y = cy + radius * sin(angle * 3.14159 / 180.0);
        if (x >= 0 && x < FB_WIDTH && y >= 0 && y < FB_HEIGHT) {
            fb[y * FB_WIDTH + x] = color;
        }
    }
}

/**
 * Draw fullscreen crosshair showing exact touch position
 */
void drawFullscreenCrosshair(uint16_t* fb, int cx, int cy, uint16_t color) {
    // Horizontal line across entire screen
    for (int x = 0; x < FB_WIDTH; x++) {
        if (cy >= 0 && cy < FB_HEIGHT) {
            fb[cy * FB_WIDTH + x] = color;
        }
    }
    // Vertical line across entire screen
    for (int y = 0; y < FB_HEIGHT; y++) {
        if (cx >= 0 && cx < FB_WIDTH) {
            fb[y * FB_WIDTH + cx] = color;
        }
    }
    // Draw center circle for emphasis
    for (int dy = -5; dy <= 5; dy++) {
        for (int dx = -5; dx <= 5; dx++) {
            if (dx*dx + dy*dy <= 25) {
                int px = cx + dx;
                int py = cy + dy;
                if (px >= 0 && px < FB_WIDTH && py >= 0 && py < FB_HEIGHT) {
                    fb[py * FB_WIDTH + px] = color;
                }
            }
        }
    }
}

/**
 * Clear entire screen to a solid color
 */
void clearScreen(uint16_t* fb, uint16_t color) {
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
        fb[i] = color;
    }
}

/**
 * Transform display coordinates to framebuffer coordinates based on orientation
 */
void displayToFramebuffer(int displayX, int displayY, int orientation,
                          int displayW, int displayH,
                          int& fbX, int& fbY) {
    switch (orientation) {
        case 0:   // Landscape (480x320 display)
            fbX = displayY;
            fbY = 479 - displayX;
            break;
        case 90:  // Portrait (320x480 display)
            fbX = displayX;
            fbY = displayY;
            break;
        case 180: // Landscape inverted (480x320 display)
            fbX = 319 - displayY;
            fbY = displayX;
            break;
        case 270: // Portrait inverted (320x480 display)
            fbX = 319 - displayX;
            fbY = 479 - displayY;
            break;
    }
}

/**
 * Transform display coordinates to native touch coordinates (always 320x480 portrait)
 * This reverses the orientation transformation that the touch driver applies
 */
void displayToNative(int displayX, int displayY, int orientation,
                     int displayW, int displayH,
                     int& nativeX, int& nativeY) {
    switch (orientation) {
        case 0:   // Landscape (480x320 display) -> Native portrait (320x480)
            nativeX = displayY;
            nativeY = 479 - displayX;
            break;
        case 90:  // Portrait (320x480 display) -> Native portrait (320x480)
            nativeX = displayX;
            nativeY = displayY;
            break;
        case 180: // Landscape inverted (480x320 display) -> Native portrait (320x480)
            nativeX = 319 - displayY;
            nativeY = displayX;
            break;
        case 270: // Portrait inverted (320x480 display) -> Native portrait (320x480)
            nativeX = 319 - displayX;
            nativeY = 479 - displayY;
            break;
    }
}

/**
 * Calibration point structure
 */
struct CalPoint {
    int displayX, displayY; // Expected display position (visual, orientation-aware)
    int nativeX, nativeY;   // Expected native touch position (always 320x480 portrait)
    int rawX, rawY;         // Measured raw ADC values
    const char* name;       // Point name for display
};

/**
 * Remove a key from settings.conf
 */
bool removeConfigKey(const std::string& configPath, const std::string& key) {
    std::ifstream inFile(configPath);
    if (!inFile.is_open()) {
        return false;  // File doesn't exist yet, that's ok
    }

    std::ostringstream buffer;
    std::string line;

    while (std::getline(inFile, line)) {
        // Skip lines that start with this key
        size_t pos = line.find(key);
        if (pos == 0 && line.find('=') != std::string::npos) {
            // This is our key - skip it (don't add to buffer)
            continue;
        }
        buffer << line << std::endl;
    }
    inFile.close();

    // Write back to file
    std::ofstream outFile(configPath);
    if (!outFile.is_open()) {
        return false;
    }
    outFile << buffer.str();
    outFile.close();

    return true;
}

/**
 * Update settings.conf with new calibration values
 */
bool updateConfigFile(const std::string& configPath, const std::string& key, int value) {
    std::ifstream inFile(configPath);
    if (!inFile.is_open()) {
        std::cerr << "[CALIBRATE] Error: Could not open " << configPath << std::endl;
        return false;
    }

    std::ostringstream buffer;
    bool found = false;
    std::string line;

    while (std::getline(inFile, line)) {
        // Check if this line contains our key
        size_t pos = line.find(key);
        if (pos == 0 && line.find('=') != std::string::npos) {
            // This is our key - replace it
            buffer << key << " = " << value << std::endl;
            found = true;
        } else {
            buffer << line << std::endl;
        }
    }
    inFile.close();

    // If key wasn't found, append it
    if (!found) {
        buffer << key << " = " << value << std::endl;
    }

    // Write back to file
    std::ofstream outFile(configPath);
    if (!outFile.is_open()) {
        std::cerr << "[CALIBRATE] Error: Could not write to " << configPath << std::endl;
        return false;
    }
    outFile << buffer.str();
    outFile.close();

    return true;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Parse command-line arguments for individual point recalibration
    int recalPoint = -1;   // -1 = calibrate all points
    int targetPoint = -1;  // -1 = no target mode, >=0 = fine-tune with 3 sub-points
    bool manualMode = false; // Manual adjustment mode with fullscreen crosshair

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]\n";
            std::cout << "Options:\n";
            std::cout << "  --recal-0 to --recal-8    Recalibrate specific point (0-8)\n";
            std::cout << "  --target-0 to --target-8  Fine-tune point with 3 sub-samples around target\n";
            std::cout << "  --manual                  Manual adjustment mode with fullscreen crosshair\n";
            std::cout << "  --help, -h                Show this help message\n";
            std::cout << "\nPoint Grid Layout:\n";
            std::cout << "  0   1   2\n";
            std::cout << "  3   4   5\n";
            std::cout << "  6   7   8\n";
            std::cout << "\nTarget Mode:\n";
            std::cout << "  Collects center point + 3 points in triangle pattern around it\n";
            std::cout << "  for better non-linear correction in specific areas\n";
            std::cout << "\nManual Mode:\n";
            std::cout << "  Shows fullscreen crosshair at touch point for visual verification\n";
            std::cout << "  After each point, shows offset and allows manual adjustment\n";
            std::cout << "  Enter X,Y offset to adjust (e.g., '-5,3' or '0,0' to accept)\n";
            return 0;
        } else if (arg == "--manual") {
            manualMode = true;
        } else if (arg.find("--recal-") == 0) {
            try {
                recalPoint = std::stoi(arg.substr(8));
                if (recalPoint < 0 || recalPoint > 8) {
                    std::cerr << "Error: Point must be 0-8\n";
                    return 1;
                }
            } catch (...) {
                std::cerr << "Error: Invalid point number\n";
                return 1;
            }
        } else if (arg.find("--target-") == 0) {
            try {
                targetPoint = std::stoi(arg.substr(9));
                if (targetPoint < 0 || targetPoint > 8) {
                    std::cerr << "Error: Target point must be 0-8\n";
                    return 1;
                }
            } catch (...) {
                std::cerr << "Error: Invalid target number\n";
                return 1;
            }
        }
    }

    std::cout << R"(
╔══════════════════════════════════════════════════════╗
║   XPT2046 Visual Touch Calibration V3                ║
║   Direct Hardware Access - No Daemon Required!       ║
╚══════════════════════════════════════════════════════╝
)" << std::endl;

    if (manualMode) {
        std::cout << "[CALIBRATE] MANUAL MODE: Fullscreen crosshair + manual adjustment enabled" << std::endl;
    }
    if (targetPoint >= 0) {
        std::cout << "[CALIBRATE] Target mode: Fine-tuning point " << targetPoint << " with 3 sub-samples" << std::endl;
    } else if (recalPoint >= 0) {
        std::cout << "[CALIBRATE] Recalibrating point " << recalPoint << " only" << std::endl;
    } else {
        std::cout << "[CALIBRATE] Full 9-point calibration" << std::endl;
    }

    std::cout << "[CALIBRATE] Initializing hardware..." << std::endl;

    // Load config for display orientation (try multiple paths)
    Config config;
    const char* configPaths[] = {
        "../../Drivers/settings.conf",  // From Apps/Touch/
        "./Drivers/settings.conf",      // From project root
        "Drivers/settings.conf"         // From project root (alternate)
    };

    std::string successfulConfigPath;  // Store which path worked
    bool configLoaded = false;
    for (const char* path : configPaths) {
        if (config.load(path)) {
            std::cout << "[CALIBRATE] Loaded config from: " << path << std::endl;
            successfulConfigPath = path;  // Remember this path for saving
            configLoaded = true;
            break;
        }
    }

    if (!configLoaded) {
        std::cerr << "[CALIBRATE] WARNING: Could not find settings.conf, using defaults" << std::endl;
        successfulConfigPath = "Drivers/settings.conf";  // Default fallback
    }

    int orientation = config.getInt("orientation", 0);
    uint32_t spiSpeed = config.getUInt("spi_speed", 32000000);

    std::cout << "[CALIBRATE] Display orientation: " << orientation << "°" << std::endl;
    std::cout << "[CALIBRATE] SPI speed: " << (spiSpeed / 1000000) << " MHz" << std::endl;

    // Initialize ILI9486 LCD (Direct Hardware Access)
    std::cout << "[CALIBRATE] Initializing ILI9486 LCD..." << std::endl;
    ILI9486 lcd("/dev/spidev0.0", 24, 25, spiSpeed);  // GPIO24=DC, GPIO25=RST, SPI speed

    if (!lcd.init()) {
        std::cerr << "[CALIBRATE] ERROR: Failed to initialize LCD!" << std::endl;
        std::cerr << "[CALIBRATE] Make sure you're running with sudo" << std::endl;
        return 1;
    }

    lcd.setOrientation(orientation);
    std::cout << "[CALIBRATE] LCD initialized successfully" << std::endl;

    // Allocate local framebuffer (we control the display directly)
    uint16_t* fb = new uint16_t[FB_WIDTH * FB_HEIGHT];
    if (!fb) {
        std::cerr << "[CALIBRATE] ERROR: Failed to allocate framebuffer!" << std::endl;
        return 1;
    }

    // Test display with blue flash
    std::cout << "[CALIBRATE] Testing display (blue flash)..." << std::endl;
    clearScreen(fb, COLOR_BLUE);
    lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
    usleep(500000);  // 500ms

    clearScreen(fb, COLOR_BLACK);
    lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
    usleep(300000);  // 300ms

    std::cout << "[CALIBRATE] Display test complete" << std::endl;

    // Setup touch controller with wide-open calibration
    // (We'll calibrate it properly during this process)
    TouchConfig touchCfg;
    touchCfg.spiDevice = "/dev/spidev0.1";
    touchCfg.spiSpeed = 2000000;  // 2MHz for XPT2046
    touchCfg.orientation = orientation;
    touchCfg.nativeWidth = 320;    // XPT2046 is portrait native
    touchCfg.nativeHeight = 480;

    // Set display dimensions based on orientation
    if (orientation == 0 || orientation == 180) {
        // Landscape
        touchCfg.displayWidth = 480;
        touchCfg.displayHeight = 320;
    } else {
        // Portrait
        touchCfg.displayWidth = 320;
        touchCfg.displayHeight = 480;
    }

    // Start with full ADC range (will be refined during calibration)
    touchCfg.numCalPoints = 0;  // No calibration yet
    touchCfg.calXMin = 0;
    touchCfg.calXMax = 4095;
    touchCfg.calYMin = 0;
    touchCfg.calYMax = 4095;
    touchCfg.pressureThreshold = 2500;  // Accept most touches
    touchCfg.pollRateHz = 100;

    // Initialize touch controller
    std::cout << "[CALIBRATE] Initializing XPT2046 touch controller..." << std::endl;
    TouchXPT2046 touch(touchCfg);

    if (!touch.init()) {
        std::cerr << "[CALIBRATE] ERROR: Failed to initialize touch controller!" << std::endl;
        std::cerr << "[CALIBRATE] Make sure XPT2046 is connected to /dev/spidev0.1" << std::endl;
        delete[] fb;
        return 1;
    }

    std::cout << "[CALIBRATE] Touch controller initialized successfully" << std::endl;

    std::cout << "\n[CALIBRATE] ════════════════════════════════════════" << std::endl;
    std::cout << "[CALIBRATE] Interactive calibration starting..." << std::endl;
    std::cout << "[CALIBRATE] Touch each crosshair as it appears" << std::endl;
    std::cout << "[CALIBRATE] Press Ctrl+C to cancel" << std::endl;
    std::cout << "[CALIBRATE] ════════════════════════════════════════\n" << std::endl;

    // Get display dimensions based on orientation
    int displayW = touchCfg.displayWidth;
    int displayH = touchCfg.displayHeight;

    // Define calibration points in DISPLAY coordinates (visual space)
    // Using 9-point calibration for better non-linear correction
    // Smaller margin = more aggressive calibration (better for worn touchscreens)
    const int margin = 15;  // Reduced from 30px to 15px for better edge coverage

    // Calculate native touch coordinates for each display position
    int nativeX[9], nativeY[9];
    int displayPositions[9][2] = {
        {margin, margin},                           // 0: Top-Left
        {displayW / 2, margin},                    // 1: Top-Center
        {displayW - margin, margin},               // 2: Top-Right
        {margin, displayH / 2},                    // 3: Mid-Left
        {displayW / 2, displayH / 2},              // 4: Center
        {displayW - margin, displayH / 2},         // 5: Mid-Right
        {margin, displayH - margin},               // 6: Bottom-Left
        {displayW / 2, displayH - margin},         // 7: Bottom-Center
        {displayW - margin, displayH - margin}     // 8: Bottom-Right
    };

    for (int i = 0; i < 9; i++) {
        displayToNative(displayPositions[i][0], displayPositions[i][1],
                       orientation, displayW, displayH,
                       nativeX[i], nativeY[i]);
    }

    CalPoint points[] = {
        {displayPositions[0][0], displayPositions[0][1], nativeX[0], nativeY[0], 0, 0, "Top-Left"},
        {displayPositions[1][0], displayPositions[1][1], nativeX[1], nativeY[1], 0, 0, "Top-Center"},
        {displayPositions[2][0], displayPositions[2][1], nativeX[2], nativeY[2], 0, 0, "Top-Right"},
        {displayPositions[3][0], displayPositions[3][1], nativeX[3], nativeY[3], 0, 0, "Mid-Left"},
        {displayPositions[4][0], displayPositions[4][1], nativeX[4], nativeY[4], 0, 0, "Center"},
        {displayPositions[5][0], displayPositions[5][1], nativeX[5], nativeY[5], 0, 0, "Mid-Right"},
        {displayPositions[6][0], displayPositions[6][1], nativeX[6], nativeY[6], 0, 0, "Bottom-Left"},
        {displayPositions[7][0], displayPositions[7][1], nativeX[7], nativeY[7], 0, 0, "Bottom-Center"},
        {displayPositions[8][0], displayPositions[8][1], nativeX[8], nativeY[8], 0, 0, "Bottom-Right"}
    };
    int numPoints = 9;

    // If recalibrating single point or using target mode, load existing calibration data first
    if (recalPoint >= 0 || targetPoint >= 0) {
        std::cout << "[CALIBRATE] Loading existing calibration data..." << std::endl;
        for (int i = 0; i < 9; i++) {
            std::string nativeXKey = "touch_cal_point_" + std::to_string(i) + "_native_x";
            std::string nativeYKey = "touch_cal_point_" + std::to_string(i) + "_native_y";
            std::string rawXKey = "touch_cal_point_" + std::to_string(i) + "_raw_x";
            std::string rawYKey = "touch_cal_point_" + std::to_string(i) + "_raw_y";

            points[i].rawX = config.getInt(rawXKey, 0);
            points[i].rawY = config.getInt(rawYKey, 0);
        }
        if (targetPoint >= 0) {
            std::cout << "[CALIBRATE] Existing data loaded. Point " << targetPoint << " will be fine-tuned." << std::endl;
        } else {
            std::cout << "[CALIBRATE] Existing data loaded. Only point " << recalPoint << " will be updated." << std::endl;
        }
    }

    std::cout << "[CALIBRATE] Display size: " << displayW << "x" << displayH << " (orientation " << orientation << "°)" << std::endl;
    std::cout << "[CALIBRATE] Native touch size: 320x480 (always portrait)" << std::endl;

    // Calibration loop
    for (int i = 0; i < numPoints && running; i++) {
        // Skip points we're not recalibrating or targeting
        if (recalPoint >= 0 && i != recalPoint) {
            std::cout << "[CALIBRATE] Skipping point " << i << " (" << points[i].name << ") - using existing calibration" << std::endl;
            continue;
        }
        if (targetPoint >= 0 && i != targetPoint) {
            std::cout << "[CALIBRATE] Skipping point " << i << " (" << points[i].name << ") - using existing calibration" << std::endl;
            continue;
        }
        // Transform display coordinates to framebuffer coordinates for drawing
        int fbX, fbY;
        displayToFramebuffer(points[i].displayX, points[i].displayY, orientation, displayW, displayH, fbX, fbY);

        // Draw calibration target at framebuffer position
        clearScreen(fb, COLOR_BLACK);
        drawCrosshair(fb, fbX, fbY, COLOR_RED, 30);
        drawCircle(fb, fbX, fbY, 40, COLOR_RED);

        // Update display
        lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);

        std::cout << "\n[CALIBRATE] ──────────────────────────────────────" << std::endl;
        std::cout << "[CALIBRATE] Point " << (i+1) << "/" << numPoints
                  << " - " << points[i].name << std::endl;
        std::cout << "[CALIBRATE] Display position: (" << points[i].displayX << "," << points[i].displayY << ")" << std::endl;
        std::cout << "[CALIBRATE] Framebuffer position: (" << fbX << "," << fbY << ")" << std::endl;
        std::cout << "[CALIBRATE] " << std::endl;
        std::cout << "[CALIBRATE] TIP: Move stylus in small CIRCLES around the" << std::endl;
        std::cout << "[CALIBRATE]      crosshair center for better accuracy!" << std::endl;
        std::cout << "[CALIBRATE] " << std::endl;
        std::cout << "[CALIBRATE] ──────────────────────────────────────" << std::endl;

        // Wait for touch and collect samples
        bool gotTouch = false;
        std::vector<int> rawXSamples, rawYSamples;
        const int requiredSamples = 10;

        while (!gotTouch && running) {
            TouchEvent event;
            if (touch.readTouch(event)) {
                if (event.state == TOUCH_PRESSED || event.state == TOUCH_HELD) {
                    // Get raw ADC values
                    int rawX, rawY, rawZ;
                    touch.getRawPosition(rawX, rawY, rawZ);

                    // Collect samples
                    if (rawX > 50 && rawX < 4000 && rawY > 50 && rawY < 4000 && rawZ >= 50 && rawZ < 2000) {
                        rawXSamples.push_back(rawX);
                        rawYSamples.push_back(rawY);

                        std::cout << "[CALIBRATE] Sample " << rawXSamples.size() << "/" << requiredSamples
                                  << " - Raw: X=" << rawX << " Y=" << rawY << " Z=" << rawZ
                                  << " Display: (" << event.x << "," << event.y << ")" << std::endl;

                        // Real-time visual feedback: show where touch was detected
                        if (rawXSamples.size() < requiredSamples) {
                            clearScreen(fb, COLOR_BLACK);
                            // Green crosshair at target position
                            drawCrosshair(fb, fbX, fbY, COLOR_GREEN, 30);
                            drawCircle(fb, fbX, fbY, 40, COLOR_GREEN);

                            // Cyan dot showing where touch is currently detected
                            int detectedFbX, detectedFbY;
                            displayToFramebuffer(event.x, event.y, orientation, displayW, displayH, detectedFbX, detectedFbY);
                            drawCircle(fb, detectedFbX, detectedFbY, 8, COLOR_CYAN);

                            lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                        }

                        // Check if we have enough samples
                        if (rawXSamples.size() >= requiredSamples) {
                            gotTouch = true;
                        }
                    }
                } else if (event.state == TOUCH_RELEASED && rawXSamples.size() > 0) {
                    // Touch released but we have some samples
                    if (rawXSamples.size() >= 5) {
                        gotTouch = true;  // Accept if we have at least 5 samples
                    } else {
                        // Not enough samples, reset
                        rawXSamples.clear();
                        rawYSamples.clear();
                        std::cout << "[CALIBRATE] Not enough samples, touch again" << std::endl;

                        // Restore red crosshair
                        clearScreen(fb, COLOR_BLACK);
                        drawCrosshair(fb, fbX, fbY, COLOR_RED, 30);
                        drawCircle(fb, fbX, fbY, 40, COLOR_RED);
                        lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                    }
                }
            }
            usleep(10000);  // 10ms polling
        }

        if (!running) break;

        // Average the samples
        int avgRawX = 0, avgRawY = 0;
        for (size_t j = 0; j < rawXSamples.size(); j++) {
            avgRawX += rawXSamples[j];
            avgRawY += rawYSamples[j];
        }
        avgRawX /= rawXSamples.size();
        avgRawY /= rawYSamples.size();

        // Store averaged calibration point
        points[i].rawX = avgRawX;
        points[i].rawY = avgRawY;

        std::cout << "[CALIBRATE] ✓ Point captured! Averaged raw: X=" << avgRawX
                  << " Y=" << avgRawY << " (from " << rawXSamples.size() << " samples)" << std::endl;

        // MANUAL MODE: Show fullscreen crosshair and allow manual adjustment
        if (manualMode) {
            std::cout << "\n[CALIBRATE] ═══ MANUAL ADJUSTMENT MODE ═══" << std::endl;

            // Show fullscreen crosshair at detected position for 2 seconds
            // First, transform raw to display coordinates using current calibration
            int detectedX, detectedY;
            touch.calibratePosition(avgRawX, avgRawY, detectedX, detectedY);
            int detectedDispX, detectedDispY;
            touch.transformForDisplay(detectedX, detectedY, detectedDispX, detectedDispY);

            // Transform to framebuffer coordinates
            int detectedFbX, detectedFbY;
            displayToFramebuffer(detectedDispX, detectedDispY, orientation, displayW, displayH, detectedFbX, detectedFbY);

            // Show fullscreen crosshair at where we detected the touch
            clearScreen(fb, COLOR_BLACK);
            drawFullscreenCrosshair(fb, detectedFbX, detectedFbY, COLOR_CYAN);
            // Show target position in red
            drawCrosshair(fb, fbX, fbY, COLOR_RED, 30);
            drawCircle(fb, fbX, fbY, 40, COLOR_RED);
            lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);

            // Calculate offset
            int offsetX = detectedDispX - points[i].displayX;
            int offsetY = detectedDispY - points[i].displayY;

            std::cout << "[CALIBRATE] Target:   Display(" << points[i].displayX << "," << points[i].displayY << ")" << std::endl;
            std::cout << "[CALIBRATE] Detected: Display(" << detectedDispX << "," << detectedDispY << ")" << std::endl;
            std::cout << "[CALIBRATE] Offset:   (" << (offsetX >= 0 ? "+" : "") << offsetX << "," << (offsetY >= 0 ? "+" : "") << offsetY << ") pixels" << std::endl;
            std::cout << "[CALIBRATE] " << std::endl;
            std::cout << "[CALIBRATE] CYAN crosshair = where you touched" << std::endl;
            std::cout << "[CALIBRATE] RED crosshair = target position" << std::endl;
            std::cout << "[CALIBRATE] " << std::endl;
            std::cout << "[CALIBRATE] Enter raw adjustment (e.g., '-50,20' or '0,0' to accept): ";
            std::cout.flush();

            // Read manual adjustment
            std::string input;
            std::getline(std::cin, input);

            if (!input.empty() && input != "0,0") {
                // Parse adjustment
                size_t commaPos = input.find(',');
                if (commaPos != std::string::npos) {
                    try {
                        int adjX = std::stoi(input.substr(0, commaPos));
                        int adjY = std::stoi(input.substr(commaPos + 1));

                        points[i].rawX = avgRawX + adjX;
                        points[i].rawY = avgRawY + adjY;

                        std::cout << "[CALIBRATE] ✓ Applied adjustment: Raw(" << adjX << "," << adjY << ")" << std::endl;
                        std::cout << "[CALIBRATE] ✓ New calibration: Raw(" << points[i].rawX << "," << points[i].rawY << ")" << std::endl;
                    } catch (...) {
                        std::cout << "[CALIBRATE] Invalid input, keeping original value" << std::endl;
                    }
                } else {
                    std::cout << "[CALIBRATE] Invalid format, keeping original value" << std::endl;
                }
            } else {
                std::cout << "[CALIBRATE] ✓ Keeping original calibration" << std::endl;
            }
        }

        // TARGET MODE: Collect 3 additional sub-points around this point
        if (targetPoint >= 0 && i == targetPoint) {
            std::cout << "\n[CALIBRATE] ════════════════════════════════════════" << std::endl;
            std::cout << "[CALIBRATE] TARGET MODE: Collecting 3 sub-points" << std::endl;
            std::cout << "[CALIBRATE] ════════════════════════════════════════" << std::endl;

            // Define 3 sub-points in a triangle pattern around the center
            // Distance of 8-10 pixels from center in display coordinates
            const int subOffset = 10;
            int subPoints[3][2] = {
                {points[i].displayX, points[i].displayY - subOffset},        // Top
                {points[i].displayX - subOffset, points[i].displayY + subOffset/2},  // Bottom-Left
                {points[i].displayX + subOffset, points[i].displayY + subOffset/2}   // Bottom-Right
            };
            const char* subNames[3] = {"Top", "Bottom-Left", "Bottom-Right"};
            int subRawX[3], subRawY[3];

            for (int sub = 0; sub < 3; sub++) {
                // Transform sub-point to framebuffer coordinates
                int subFbX, subFbY;
                displayToFramebuffer(subPoints[sub][0], subPoints[sub][1],
                                    orientation, displayW, displayH, subFbX, subFbY);

                // Draw sub-point target
                clearScreen(fb, COLOR_BLACK);
                // Show main point in dim blue
                drawCrosshair(fb, fbX, fbY, COLOR_BLUE, 20);
                // Show sub-point in bright yellow
                drawCrosshair(fb, subFbX, subFbY, COLOR_YELLOW, 15);
                drawCircle(fb, subFbX, subFbY, 20, COLOR_YELLOW);
                lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);

                std::cout << "\n[CALIBRATE] ──────────────────────────────────────" << std::endl;
                std::cout << "[CALIBRATE] Sub-point " << (sub+1) << "/3 - " << subNames[sub] << std::endl;
                std::cout << "[CALIBRATE] Display position: (" << subPoints[sub][0] << "," << subPoints[sub][1] << ")" << std::endl;
                std::cout << "[CALIBRATE] TIP: Move stylus in small CIRCLES" << std::endl;
                std::cout << "[CALIBRATE] ──────────────────────────────────────" << std::endl;

                // Collect sub-point samples (20 samples for fine-tuning)
                bool gotSubTouch = false;
                std::vector<int> subRawXSamples, subRawYSamples;
                const int subRequiredSamples = 20;  // More samples for finer accuracy

                while (!gotSubTouch && running) {
                    TouchEvent event;
                    if (touch.readTouch(event)) {
                        if (event.state == TOUCH_PRESSED || event.state == TOUCH_HELD) {
                            int rawX, rawY, rawZ;
                            touch.getRawPosition(rawX, rawY, rawZ);

                            if (rawX > 50 && rawX < 4000 && rawY > 50 && rawY < 4000 && rawZ >= 50 && rawZ < 2000) {
                                subRawXSamples.push_back(rawX);
                                subRawYSamples.push_back(rawY);

                                std::cout << "[CALIBRATE] Sample " << subRawXSamples.size() << "/" << subRequiredSamples
                                          << " - Raw: X=" << rawX << " Y=" << rawY << std::endl;

                                // Real-time visual feedback: show where touch was detected
                                if (subRawXSamples.size() < subRequiredSamples) {
                                    clearScreen(fb, COLOR_BLACK);
                                    // Dim blue crosshair at main point
                                    drawCrosshair(fb, fbX, fbY, COLOR_BLUE, 20);
                                    // Green crosshair at sub-point target
                                    drawCrosshair(fb, subFbX, subFbY, COLOR_GREEN, 15);
                                    drawCircle(fb, subFbX, subFbY, 20, COLOR_GREEN);

                                    // Cyan dot showing where touch is currently detected
                                    int detectedFbX, detectedFbY;
                                    displayToFramebuffer(event.x, event.y, orientation, displayW, displayH, detectedFbX, detectedFbY);
                                    drawCircle(fb, detectedFbX, detectedFbY, 5, COLOR_CYAN);

                                    lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                                }

                                if (subRawXSamples.size() >= subRequiredSamples) {
                                    gotSubTouch = true;
                                }
                            }
                        } else if (event.state == TOUCH_RELEASED && subRawXSamples.size() > 0) {
                            if (subRawXSamples.size() >= 10) {
                                gotSubTouch = true;  // Accept if we have at least 10 samples
                            } else {
                                // Not enough samples, reset
                                subRawXSamples.clear();
                                subRawYSamples.clear();
                                std::cout << "[CALIBRATE] Not enough samples, touch again" << std::endl;

                                // Restore yellow crosshair
                                clearScreen(fb, COLOR_BLACK);
                                drawCrosshair(fb, fbX, fbY, COLOR_BLUE, 20);
                                drawCrosshair(fb, subFbX, subFbY, COLOR_YELLOW, 15);
                                drawCircle(fb, subFbX, subFbY, 20, COLOR_YELLOW);
                                lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                            }
                        }
                    }
                    usleep(10000);
                }

                if (!running) break;

                // Calculate best adjusted value from 20 samples
                // Sort samples and remove top/bottom 2 outliers for better accuracy
                std::vector<int> sortedX = subRawXSamples;
                std::vector<int> sortedY = subRawYSamples;
                std::sort(sortedX.begin(), sortedX.end());
                std::sort(sortedY.begin(), sortedY.end());

                // Remove top 2 and bottom 2 outliers if we have enough samples
                int startIdx = (sortedX.size() >= 8) ? 2 : 0;
                int endIdx = (sortedX.size() >= 8) ? sortedX.size() - 2 : sortedX.size();

                int avgSubX = 0, avgSubY = 0;
                int validSamples = endIdx - startIdx;
                for (int j = startIdx; j < endIdx; j++) {
                    avgSubX += sortedX[j];
                    avgSubY += sortedY[j];
                }
                avgSubX /= validSamples;
                avgSubY /= validSamples;

                subRawX[sub] = avgSubX;
                subRawY[sub] = avgSubY;

                std::cout << "[CALIBRATE] ✓ Sub-point captured! Raw: X=" << avgSubX
                          << " Y=" << avgSubY << " (from " << subRawXSamples.size()
                          << " samples, " << validSamples << " used)" << std::endl;
            }

            if (!running) break;

            // Calculate weighted average: center gets weight 2, each sub-point gets weight 1
            // This gives finer resolution while keeping the main calibration point dominant
            int finalRawX = (avgRawX * 2 + subRawX[0] + subRawX[1] + subRawX[2]) / 5;
            int finalRawY = (avgRawY * 2 + subRawY[0] + subRawY[1] + subRawY[2]) / 5;

            points[i].rawX = finalRawX;
            points[i].rawY = finalRawY;

            std::cout << "\n[CALIBRATE] ════════════════════════════════════════" << std::endl;
            std::cout << "[CALIBRATE] Target mode complete!" << std::endl;
            std::cout << "[CALIBRATE] Center:  Raw(" << avgRawX << "," << avgRawY << ")" << std::endl;
            std::cout << "[CALIBRATE] Top:     Raw(" << subRawX[0] << "," << subRawY[0] << ")" << std::endl;
            std::cout << "[CALIBRATE] Bot-L:   Raw(" << subRawX[1] << "," << subRawY[1] << ")" << std::endl;
            std::cout << "[CALIBRATE] Bot-R:   Raw(" << subRawX[2] << "," << subRawY[2] << ")" << std::endl;
            std::cout << "[CALIBRATE] Weighted Average: Raw(" << finalRawX << "," << finalRawY << ")" << std::endl;
            std::cout << "[CALIBRATE] ════════════════════════════════════════" << std::endl;
        }

        // Brief visual feedback
        clearScreen(fb, COLOR_GREEN);
        lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
        usleep(200000);  // 200ms green flash
    }

    if (!running) {
        std::cout << "\n[CALIBRATE] Calibration cancelled by user" << std::endl;
        delete[] fb;
        return 1;
    }

    // Calculate calibration values
    std::cout << "\n[CALIBRATE] ════════════════════════════════════════" << std::endl;
    std::cout << "[CALIBRATE] Calculating 9-point calibration..." << std::endl;
    std::cout << "[CALIBRATE] ════════════════════════════════════════\n" << std::endl;

    // Display calibration summary
    std::cout << "[CALIBRATE] Calibration Summary:" << std::endl;
    std::cout << "[CALIBRATE] ────────────────────────────────────────" << std::endl;
    for (int i = 0; i < numPoints; i++) {
        std::cout << "[CALIBRATE] " << std::setw(13) << std::left << points[i].name
                  << " - Display(" << std::setw(3) << points[i].displayX << "," << std::setw(3) << points[i].displayY << ")"
                  << " Native(" << std::setw(3) << points[i].nativeX << "," << std::setw(3) << points[i].nativeY << ")"
                  << " Raw(" << std::setw(4) << points[i].rawX << "," << std::setw(4) << points[i].rawY << ")"
                  << std::endl;
    }

    std::cout << "\n[CALIBRATE] Saving 9-point calibration data..." << std::endl;
    std::cout << "[CALIBRATE] ────────────────────────────────────────" << std::endl;

    // Save to settings.conf
    std::cout << "[CALIBRATE] Saving calibration to " << successfulConfigPath << "..." << std::endl;

    bool success = true;

    // Remove old calibration values (only when doing full calibration)
    if (recalPoint < 0) {
        std::cout << "[CALIBRATE] Removing old calibration values..." << std::endl;
        removeConfigKey(successfulConfigPath, "touch_cal_x_min");
        removeConfigKey(successfulConfigPath, "touch_cal_x_max");
        removeConfigKey(successfulConfigPath, "touch_cal_y_min");
        removeConfigKey(successfulConfigPath, "touch_cal_y_max");

        // Also remove any old 9-point calibration data
        for (int i = 0; i < 9; i++) {
            std::string nativeXKey = "touch_cal_point_" + std::to_string(i) + "_native_x";
            std::string nativeYKey = "touch_cal_point_" + std::to_string(i) + "_native_y";
            std::string rawXKey = "touch_cal_point_" + std::to_string(i) + "_raw_x";
            std::string rawYKey = "touch_cal_point_" + std::to_string(i) + "_raw_y";
            removeConfigKey(successfulConfigPath, nativeXKey);
            removeConfigKey(successfulConfigPath, nativeYKey);
            removeConfigKey(successfulConfigPath, rawXKey);
            removeConfigKey(successfulConfigPath, rawYKey);
        }
    } else {
        std::cout << "[CALIBRATE] Updating point " << recalPoint << " only (preserving other points)..." << std::endl;
    }

    // Save the number of calibration points
    success &= updateConfigFile(successfulConfigPath, "touch_cal_points", 9);

    // Save display dimensions used for calibration
    success &= updateConfigFile(successfulConfigPath, "touch_cal_display_width", displayW);
    success &= updateConfigFile(successfulConfigPath, "touch_cal_display_height", displayH);

    // Save all 9 calibration points (native coords and raw ADC values)
    for (int i = 0; i < numPoints; i++) {
        std::string nativeXKey = "touch_cal_point_" + std::to_string(i) + "_native_x";
        std::string nativeYKey = "touch_cal_point_" + std::to_string(i) + "_native_y";
        std::string rawXKey = "touch_cal_point_" + std::to_string(i) + "_raw_x";
        std::string rawYKey = "touch_cal_point_" + std::to_string(i) + "_raw_y";

        success &= updateConfigFile(successfulConfigPath, nativeXKey, points[i].nativeX);
        success &= updateConfigFile(successfulConfigPath, nativeYKey, points[i].nativeY);
        success &= updateConfigFile(successfulConfigPath, rawXKey, points[i].rawX);
        success &= updateConfigFile(successfulConfigPath, rawYKey, points[i].rawY);

        std::cout << "[CALIBRATE]   Point " << i << ": Native(" << points[i].nativeX << "," << points[i].nativeY
                  << ") Raw(" << points[i].rawX << "," << points[i].rawY << ")" << std::endl;
    }

    if (success) {
        std::cout << "[CALIBRATE] ✓ Calibration values saved successfully!" << std::endl;

        // Display success message on screen
        clearScreen(fb, COLOR_GREEN);
        lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
        usleep(500000);  // 500ms green flash

        clearScreen(fb, COLOR_BLACK);
        lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);

        // ═══════════════════════════════════════════════════════════════
        // POST-CALIBRATION VALIDATION TEST
        // ═══════════════════════════════════════════════════════════════

        std::cout << "\n[CALIBRATE] ════════════════════════════════════════" << std::endl;
        std::cout << "[CALIBRATE] Starting validation test..." << std::endl;
        std::cout << "[CALIBRATE] Touch test points to verify accuracy" << std::endl;
        std::cout << "[CALIBRATE] ════════════════════════════════════════\n" << std::endl;

        // Reinitialize touch controller with new 9-point calibration
        touchCfg.numCalPoints = 9;
        for (int i = 0; i < numPoints; i++) {
            touchCfg.calPoints[i].nativeX = points[i].nativeX;
            touchCfg.calPoints[i].nativeY = points[i].nativeY;
            touchCfg.calPoints[i].rawX = points[i].rawX;
            touchCfg.calPoints[i].rawY = points[i].rawY;
        }

        std::cout << "[VALIDATE] Using 9-point bilinear interpolation" << std::endl;

        TouchXPT2046 touchCalibrated(touchCfg);
        if (!touchCalibrated.init()) {
            std::cerr << "[CALIBRATE] WARNING: Could not reinitialize touch with new calibration" << std::endl;
        } else {
            // Define validation test points (9 points total)
            struct TestPoint {
                int displayX, displayY;
                const char* name;
            };

            TestPoint testPoints[] = {
                {margin, margin, "Top-Left"},
                {displayW / 2, margin, "Top-Center"},
                {displayW - margin, margin, "Top-Right"},
                {margin, displayH / 2, "Mid-Left"},
                {displayW / 2, displayH / 2, "Center"},
                {displayW - margin, displayH / 2, "Mid-Right"},
                {margin, displayH - margin, "Bottom-Left"},
                {displayW / 2, displayH - margin, "Bottom-Center"},
                {displayW - margin, displayH - margin, "Bottom-Right"}
            };
            int numTestPoints = 9;

            int totalOffsetX = 0, totalOffsetY = 0;
            int maxOffset = 0;

            for (int i = 0; i < numTestPoints && running; i++) {
                // Transform to framebuffer coordinates for drawing
                int fbTestX, fbTestY;
                displayToFramebuffer(testPoints[i].displayX, testPoints[i].displayY,
                                    orientation, displayW, displayH, fbTestX, fbTestY);

                // Draw test crosshair
                clearScreen(fb, COLOR_BLACK);
                drawCrosshair(fb, fbTestX, fbTestY, COLOR_YELLOW, 20);
                drawCircle(fb, fbTestX, fbTestY, 30, COLOR_YELLOW);
                lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);

                std::cout << "\n[VALIDATE] ──────────────────────────────────────" << std::endl;
                std::cout << "[VALIDATE] Test " << (i+1) << "/" << numTestPoints
                          << " - " << testPoints[i].name << std::endl;
                std::cout << "[VALIDATE] Expected: (" << testPoints[i].displayX
                          << "," << testPoints[i].displayY << ")" << std::endl;
                std::cout << "[VALIDATE] Touch the YELLOW crosshair..." << std::endl;

                // Wait for touch
                bool gotTestTouch = false;
                int detectedX = 0, detectedY = 0;

                while (!gotTestTouch && running) {
                    TouchEvent testEvent;
                    if (touchCalibrated.readTouch(testEvent)) {
                        if (testEvent.state == TOUCH_PRESSED) {
                            detectedX = testEvent.x;
                            detectedY = testEvent.y;

                            // Show cyan dot where touch was detected
                            int detectedFbX, detectedFbY;
                            displayToFramebuffer(detectedX, detectedY, orientation,
                                               displayW, displayH, detectedFbX, detectedFbY);

                            clearScreen(fb, COLOR_BLACK);
                            drawCrosshair(fb, fbTestX, fbTestY, COLOR_YELLOW, 20);
                            drawCircle(fb, fbTestX, fbTestY, 30, COLOR_YELLOW);
                            drawCircle(fb, detectedFbX, detectedFbY, 10, COLOR_CYAN);
                            lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);

                            gotTestTouch = true;
                            usleep(300000);  // Show for 300ms
                        }
                    }
                    usleep(10000);
                }

                if (!running) break;

                // Calculate offset
                int offsetX = detectedX - testPoints[i].displayX;
                int offsetY = detectedY - testPoints[i].displayY;
                int offsetMagnitude = sqrt(offsetX * offsetX + offsetY * offsetY);

                totalOffsetX += abs(offsetX);
                totalOffsetY += abs(offsetY);
                if (offsetMagnitude > maxOffset) maxOffset = offsetMagnitude;

                std::cout << "[VALIDATE] Detected: (" << detectedX << "," << detectedY << ")" << std::endl;
                std::cout << "[VALIDATE] Offset: (" << offsetX << "," << offsetY
                          << ") = " << offsetMagnitude << " pixels" << std::endl;

                // Color-code accuracy
                if (offsetMagnitude <= 10) {
                    std::cout << "[VALIDATE] ✓ Excellent accuracy!" << std::endl;
                } else if (offsetMagnitude <= 20) {
                    std::cout << "[VALIDATE] ✓ Good accuracy" << std::endl;
                } else if (offsetMagnitude <= 35) {
                    std::cout << "[VALIDATE] ⚠ Acceptable accuracy" << std::endl;
                } else {
                    std::cout << "[VALIDATE] ✗ Poor accuracy - may need recalibration" << std::endl;
                }
            }

            if (running) {
                // Display validation summary
                float avgOffsetX = totalOffsetX / (float)numTestPoints;
                float avgOffsetY = totalOffsetY / (float)numTestPoints;

                clearScreen(fb, COLOR_BLACK);
                lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);

                std::cout << "\n[VALIDATE] ════════════════════════════════════════" << std::endl;
                std::cout << "[VALIDATE] Validation Complete!" << std::endl;
                std::cout << "[VALIDATE] ────────────────────────────────────────" << std::endl;
                std::cout << "[VALIDATE] Average offset: " << avgOffsetX << "x, " << avgOffsetY << "y pixels" << std::endl;
                std::cout << "[VALIDATE] Maximum offset: " << maxOffset << " pixels" << std::endl;

                if (maxOffset <= 20) {
                    std::cout << "[VALIDATE] ✓ Calibration is EXCELLENT!" << std::endl;
                    clearScreen(fb, COLOR_GREEN);
                    lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                    usleep(500000);
                } else if (maxOffset <= 35) {
                    std::cout << "[VALIDATE] ✓ Calibration is GOOD" << std::endl;
                    clearScreen(fb, COLOR_BLUE);
                    lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                    usleep(500000);
                } else {
                    std::cout << "[VALIDATE] ⚠ Calibration accuracy is POOR" << std::endl;
                    std::cout << "[VALIDATE] Consider running calibration again" << std::endl;
                    clearScreen(fb, COLOR_YELLOW);
                    lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                    usleep(1000000);
                }

                clearScreen(fb, COLOR_BLACK);
                lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                std::cout << "[VALIDATE] ════════════════════════════════════════\n" << std::endl;
            }
        }

        // ═══════════════════════════════════════════════════════════════

        std::cout << "\n[CALIBRATE] ════════════════════════════════════════" << std::endl;
        std::cout << "[CALIBRATE] Calibration complete!" << std::endl;
        std::cout << "[CALIBRATE] You can now test touch accuracy with:" << std::endl;
        std::cout << "[CALIBRATE]   sudo ./Apps/Touch/touch_draw_v3" << std::endl;
        std::cout << "[CALIBRATE] ════════════════════════════════════════\n" << std::endl;
    } else {
        std::cerr << "[CALIBRATE] ERROR: Failed to save calibration!" << std::endl;
        std::cerr << "[CALIBRATE] Check file permissions for " << successfulConfigPath << std::endl;

        clearScreen(fb, COLOR_RED);
        lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
        usleep(1000000);  // 1s red flash

        delete[] fb;
        return 1;
    }

    // Cleanup
    delete[] fb;
    return 0;
}
