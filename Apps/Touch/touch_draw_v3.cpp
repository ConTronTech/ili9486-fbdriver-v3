/**
 * Touch Drawing App V3 - Direct Hardware Access
 *
 * Free-form touch drawing application for testing touch accuracy and responsiveness.
 * Uses Direct Hardware Access pattern - NO DAEMON REQUIRED!
 *
 * Features:
 * - Draw continuous lines by dragging finger
 * - Color palette (tap top edge to change colors)
 * - Clear button (tap top-center)
 * - Demonstrates proper TOUCH_HELD throttling for smooth drawing
 * - Real-time FPS display
 *
 * Usage: sudo ./touch_draw_v3
 *
 * Requirements:
 * - Root access (for SPI hardware)
 * - ILI9486 on /dev/spidev0.0
 * - XPT2046 on /dev/spidev0.1
 * - Calibrated touch (run touch_calibrate_visual_v3 first)
 */

#include <iostream>
#include <csignal>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include "../../Drivers/ili9486.h"
#include "../../Drivers/touch_xpt2046_v3.h"
#include "../../Drivers/config.h"

// Framebuffer dimensions (ALWAYS 320x480 portrait in memory, MADCTL handles rotation)
#define FB_WIDTH 320
#define FB_HEIGHT 480

// Frame rate throttling for TOUCH_HELD events (milliseconds)
// Limits buffer updates to ~30 FPS to prevent saturation during drag
#define DRAW_THROTTLE_MS 33

// Colors (RGB565)
#define COLOR_BLACK      0x0000
#define COLOR_WHITE      0xFFFF
#define COLOR_RED        0xF800
#define COLOR_GREEN      0x07E0
#define COLOR_BLUE       0x001F
#define COLOR_YELLOW     0xFFE0
#define COLOR_CYAN       0x07FF
#define COLOR_MAGENTA    0xF81F
#define COLOR_ORANGE     0xFD20
#define COLOR_DARK_GRAY  0x4208

// Color palette
struct ColorPalette {
    uint16_t color;
    const char* name;
};

const ColorPalette COLORS[] = {
    {COLOR_WHITE, "White"},
    {COLOR_RED, "Red"},
    {COLOR_GREEN, "Green"},
    {COLOR_BLUE, "Blue"},
    {COLOR_YELLOW, "Yellow"},
    {COLOR_CYAN, "Cyan"},
    {COLOR_MAGENTA, "Magenta"},
    {COLOR_ORANGE, "Orange"}
};
const int NUM_COLORS = sizeof(COLORS) / sizeof(COLORS[0]);

static volatile bool running = true;
static int g_orientation = 0;  // Global orientation for coordinate transforms
static bool debugMode = false;  // Show crosshairs for calibration debugging (use --debug to enable)

void signalHandler(int) {
    running = false;
}

/**
 * Transform display coordinates to framebuffer coordinates based on orientation
 * Display coords are in visual space (e.g., 480x320 for landscape)
 * Framebuffer is always 320x480 portrait in memory
 */
void displayToFramebuffer(int displayX, int displayY, int& fbX, int& fbY) {
    switch (g_orientation) {
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
        default:
            fbX = displayX;
            fbY = displayY;
            break;
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
 * Draw a single pixel (in framebuffer coordinates)
 */
inline void drawPixelFB(uint16_t* fb, int fbX, int fbY, uint16_t color) {
    if (fbX >= 0 && fbX < FB_WIDTH && fbY >= 0 && fbY < FB_HEIGHT) {
        fb[fbY * FB_WIDTH + fbX] = color;
    }
}

/**
 * Draw a single pixel (in display coordinates - transforms to framebuffer)
 */
inline void drawPixel(uint16_t* fb, int displayX, int displayY, uint16_t color) {
    int fbX, fbY;
    displayToFramebuffer(displayX, displayY, fbX, fbY);
    drawPixelFB(fb, fbX, fbY, color);
}

/**
 * Draw a filled circle (in display coordinates)
 */
void drawCircle(uint16_t* fb, int cx, int cy, int radius, uint16_t color) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx*dx + dy*dy <= radius*radius) {
                drawPixel(fb, cx + dx, cy + dy, color);
            }
        }
    }
}

/**
 * Draw a line using Bresenham's algorithm (in display coordinates)
 */
void drawLine(uint16_t* fb, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        drawPixel(fb, x0, y0, color);

        if (x0 == x1 && y0 == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/**
 * Draw a filled rectangle (in display coordinates)
 */
void drawRect(uint16_t* fb, int x, int y, int w, int h, uint16_t color) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            drawPixel(fb, px, py, color);
        }
    }
}

/**
 * Draw a crosshair at the given position (for debug mode)
 */
void drawCrosshair(uint16_t* fb, int x, int y, uint16_t color, int size = 10) {
    // Horizontal line
    for (int i = -size; i <= size; i++) {
        drawPixel(fb, x + i, y, color);
    }
    // Vertical line
    for (int i = -size; i <= size; i++) {
        drawPixel(fb, x, y + i, color);
    }
    // Center dot
    drawCircle(fb, x, y, 2, color);
}

/**
 * Get display dimensions based on orientation
 */
void getDisplayDimensions(int& displayWidth, int& displayHeight) {
    if (g_orientation == 0 || g_orientation == 180) {
        displayWidth = 480;
        displayHeight = 320;
    } else {
        displayWidth = 320;
        displayHeight = 480;
    }
}

/**
 * Draw the color palette bar at top of screen
 */
void drawColorPalette(uint16_t* fb, int currentColorIndex) {
    int displayWidth, displayHeight;
    getDisplayDimensions(displayWidth, displayHeight);

    const int barHeight = 40;  // Increased from 30 to make it easier to tap
    const int swatchWidth = displayWidth / NUM_COLORS;

    for (int i = 0; i < NUM_COLORS; i++) {
        int x = i * swatchWidth;

        // If selected, make it slightly larger/raised for better visibility
        if (i == currentColorIndex) {
            // Draw selected color with thicker border and filled indicator
            drawRect(fb, x, 0, swatchWidth, barHeight, COLORS[i].color);

            // Draw thick black border for contrast
            drawRect(fb, x, 0, swatchWidth, 4, COLOR_BLACK);
            drawRect(fb, x, barHeight - 4, swatchWidth, 4, COLOR_BLACK);
            drawRect(fb, x, 0, 4, barHeight, COLOR_BLACK);
            drawRect(fb, x + swatchWidth - 4, 0, 4, barHeight, COLOR_BLACK);

            // Draw inner white border for emphasis
            drawRect(fb, x + 4, 4, swatchWidth - 8, 2, COLOR_WHITE);
            drawRect(fb, x + 4, barHeight - 6, swatchWidth - 8, 2, COLOR_WHITE);
            drawRect(fb, x + 4, 4, 2, barHeight - 8, COLOR_WHITE);
            drawRect(fb, x + swatchWidth - 6, 4, 2, barHeight - 8, COLOR_WHITE);
        } else {
            // Normal unselected color
            drawRect(fb, x, 0, swatchWidth, barHeight, COLORS[i].color);
            // Subtle border between swatches
            drawRect(fb, x + swatchWidth - 1, 0, 1, barHeight, COLOR_DARK_GRAY);
        }
    }

    // Draw separator line
    drawRect(fb, 0, barHeight, displayWidth, 2, COLOR_WHITE);
}

/**
 * Draw clear button below palette
 */
void drawClearButton(uint16_t* fb) {
    int displayWidth, displayHeight;
    getDisplayDimensions(displayWidth, displayHeight);

    const int paletteHeight = 40;
    const int btnWidth = 80;   // Made wider for easier tapping
    const int btnHeight = 25;  // Made taller for easier tapping
    const int btnX = (displayWidth - btnWidth) / 2;
    const int btnY = paletteHeight + 5;  // Just below palette

    // Button background
    drawRect(fb, btnX, btnY, btnWidth, btnHeight, COLOR_RED);
    // Button border (thicker for better visibility)
    drawRect(fb, btnX, btnY, btnWidth, 3, COLOR_WHITE);
    drawRect(fb, btnX, btnY + btnHeight - 3, btnWidth, 3, COLOR_WHITE);
    drawRect(fb, btnX, btnY, 3, btnHeight, COLOR_WHITE);
    drawRect(fb, btnX + btnWidth - 3, btnY, 3, btnHeight, COLOR_WHITE);
}

/**
 * Get color index from X coordinate in palette area
 */
int getColorFromPaletteX(int x) {
    int displayWidth, displayHeight;
    getDisplayDimensions(displayWidth, displayHeight);

    const int swatchWidth = displayWidth / NUM_COLORS;
    int colorIndex = x / swatchWidth;
    if (colorIndex < 0) colorIndex = 0;
    if (colorIndex >= NUM_COLORS) colorIndex = NUM_COLORS - 1;
    return colorIndex;
}

/**
 * Check if touch is in clear button area
 */
bool isTouchInClearButton(int x, int y) {
    int displayWidth, displayHeight;
    getDisplayDimensions(displayWidth, displayHeight);

    const int paletteHeight = 40;
    const int btnWidth = 80;
    const int btnHeight = 25;
    const int btnX = (displayWidth - btnWidth) / 2;
    const int btnY = paletteHeight + 5;

    return (x >= btnX && x < btnX + btnWidth &&
            y >= btnY && y < btnY + btnHeight);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--debug" || arg == "-d") {
            debugMode = true;
            std::cout << "[DRAW] Debug mode enabled - crosshairs will show touch position" << std::endl;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]\n\n";
            std::cout << "Touch Drawing App - Test touch accuracy and have fun!\n\n";
            std::cout << "Options:\n";
            std::cout << "  --debug, -d    Enable debug mode (show crosshairs at touch position)\n";
            std::cout << "  --help, -h     Show this help message\n\n";
            std::cout << "Controls:\n";
            std::cout << "  - Tap color bar to change color\n";
            std::cout << "  - Draw with your finger\n";
            std::cout << "  - Tap 'Clear' button to erase\n";
            std::cout << "  - Press Ctrl+C to exit\n";
            return 0;
        } else {
            std::cerr << "[DRAW] Unknown option: " << arg << std::endl;
            std::cerr << "[DRAW] Use --help for usage information" << std::endl;
            return 1;
        }
    }

    std::cout << R"(
╔══════════════════════════════════════════════════════╗
║   Touch Drawing App V3                               ║
║   Draw with your finger - test touch accuracy!      ║
╚══════════════════════════════════════════════════════╝
)" << std::endl;

    std::cout << "[DRAW] Initializing hardware..." << std::endl;

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
            std::cout << "[DRAW] Loaded config from: " << path << std::endl;
            configLoaded = true;
            break;
        }
    }

    if (!configLoaded) {
        std::cerr << "[DRAW] ERROR: Could not find settings.conf!" << std::endl;
        std::cerr << "[DRAW] Tried:" << std::endl;
        for (const char* path : configPaths) {
            std::cerr << "[DRAW]   - " << path << std::endl;
        }
        std::cerr << "[DRAW] Please run from Apps/Touch/ directory or check file exists" << std::endl;
        return 1;
    }

    g_orientation = config.getInt("orientation", 0);
    uint32_t spiSpeed = config.getUInt("spi_speed", 32000000);

    // Get display dimensions based on orientation
    int displayWidth, displayHeight;
    getDisplayDimensions(displayWidth, displayHeight);

    std::cout << "[DRAW] Display orientation: " << g_orientation << "° (" << displayWidth << "x" << displayHeight << ")" << std::endl;
    std::cout << "[DRAW] Framebuffer: " << FB_WIDTH << "x" << FB_HEIGHT << " (always portrait in memory)" << std::endl;

    // Initialize ILI9486 LCD
    std::cout << "[DRAW] Initializing ILI9486 LCD..." << std::endl;
    ILI9486 lcd("/dev/spidev0.0", 24, 25, spiSpeed);

    if (!lcd.init()) {
        std::cerr << "[DRAW] ERROR: Failed to initialize LCD!" << std::endl;
        std::cerr << "[DRAW] Make sure you're running with sudo" << std::endl;
        return 1;
    }

    lcd.setOrientation(g_orientation);
    std::cout << "[DRAW] LCD initialized (Orientation: " << g_orientation << "°)" << std::endl;

    // Allocate framebuffer (always 320x480 = 153,600 pixels)
    uint16_t* fb = new (std::nothrow) uint16_t[FB_WIDTH * FB_HEIGHT];
    if (!fb) {
        std::cerr << "[DRAW] ERROR: Failed to allocate framebuffer!" << std::endl;
        return 1;
    }

    // Setup touch controller with DISPLAY dimensions
    TouchConfig touchCfg;
    touchCfg.spiDevice = config.getString("touch_spi_device", "/dev/spidev0.1");
    touchCfg.spiSpeed = config.getUInt("touch_spi_speed", 2000000);
    touchCfg.orientation = g_orientation;
    touchCfg.nativeWidth = config.getInt("touch_native_width", 320);
    touchCfg.nativeHeight = config.getInt("touch_native_height", 480);
    touchCfg.displayWidth = displayWidth;   // Use display dimensions (e.g., 480x320 for landscape)
    touchCfg.displayHeight = displayHeight;
    // Load legacy calibration (for fallback)
    touchCfg.calXMin = config.getInt("touch_cal_x_min", 200);
    touchCfg.calXMax = config.getInt("touch_cal_x_max", 3900);
    touchCfg.calYMin = config.getInt("touch_cal_y_min", 200);
    touchCfg.calYMax = config.getInt("touch_cal_y_max", 3900);

    // Load 9-point calibration if available
    touchCfg.numCalPoints = config.getInt("touch_cal_points", 0);
    if (touchCfg.numCalPoints == 9) {
        std::cout << "[DRAW] Loading 9-point calibration..." << std::endl;
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
        std::cout << "[DRAW] ✓ 9-point calibration loaded" << std::endl;
    } else {
        std::cout << "[DRAW] Using legacy 4-point calibration" << std::endl;
    }

    touchCfg.pressureThreshold = config.getUInt("touch_pressure_threshold", 1200);
    touchCfg.pollRateHz = config.getInt("touch_poll_rate", 100);

    // Initialize touch controller
    std::cout << "[DRAW] Initializing XPT2046 touch controller..." << std::endl;
    TouchXPT2046 touch(touchCfg);

    if (!touch.init()) {
        std::cerr << "[DRAW] ERROR: Failed to initialize touch controller!" << std::endl;
        std::cerr << "[DRAW] Make sure XPT2046 is connected to " << touchCfg.spiDevice << std::endl;
        delete[] fb;
        return 1;
    }

    std::cout << "[DRAW] Touch controller initialized" << std::endl;

    std::cout << "\n[DRAW] ════════════════════════════════════════════" << std::endl;
    if (debugMode) {
        std::cout << "[DRAW] Drawing app ready! (DEBUG MODE)" << std::endl;
    } else {
        std::cout << "[DRAW] Drawing app ready!" << std::endl;
    }
    std::cout << "[DRAW] • Drag finger to draw" << std::endl;
    std::cout << "[DRAW] • Tap top edge to change color" << std::endl;
    std::cout << "[DRAW] • Tap center button to clear" << std::endl;
    std::cout << "[DRAW] • Press Ctrl+C to exit" << std::endl;
    std::cout << "[DRAW] ════════════════════════════════════════════" << std::endl;

    if (debugMode) {
        std::cout << "[DRAW] Debug Mode: CYAN crosshairs show touch position" << std::endl;
        std::cout << "[DRAW] Watch console for coordinate mapping" << std::endl;
        std::cout << "[DRAW] ────────────────────────────────────────────" << std::endl;
    }

    std::cout << "[DRAW] Current calibration:" << std::endl;
    if (touchCfg.numCalPoints == 9) {
        std::cout << "[DRAW]   Type: 9-point bilinear interpolation" << std::endl;
        std::cout << "[DRAW]   Grid: 3x3 calibration points" << std::endl;
    } else {
        std::cout << "[DRAW]   Type: Legacy 4-point linear" << std::endl;
        std::cout << "[DRAW]   X: " << touchCfg.calXMin << " to " << touchCfg.calXMax << std::endl;
        std::cout << "[DRAW]   Y: " << touchCfg.calYMin << " to " << touchCfg.calYMax << std::endl;
    }
    std::cout << "[DRAW]   Display: " << displayWidth << "x" << displayHeight << std::endl;

    if (debugMode) {
        std::cout << "[DRAW]   Debug: ENABLED (use regular run to disable)" << std::endl;
    }

    std::cout << "[DRAW] ════════════════════════════════════════════\n" << std::endl;

    // Initialize canvas
    clearScreen(fb, COLOR_BLACK);
    int currentColorIndex = 0;  // Start with white
    drawColorPalette(fb, currentColorIndex);
    drawClearButton(fb);
    lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);

    // Drawing state
    int lastTouchX = -1;
    int lastTouchY = -1;
    auto lastDrawTime = std::chrono::steady_clock::now();
    auto lastFPSTime = std::chrono::steady_clock::now();
    uint32_t frameCount = 0;
    uint32_t currentFPS = 0;

    // Dirty region tracking for partial updates
    int dirtyMinX = 0, dirtyMinY = 0, dirtyMaxX = 0, dirtyMaxY = 0;
    bool hasDirtyRegion = false;

    // Main drawing loop
    while (running) {
        TouchEvent event;
        if (touch.readTouch(event)) {
            if (event.state == TOUCH_PRESSED) {
                // Check if touch is in palette area (larger for easier selection)
                if (event.y < 40) {
                    // Color palette selection
                    currentColorIndex = getColorFromPaletteX(event.x);
                    std::cout << "[DRAW] Color changed to: " << COLORS[currentColorIndex].name << std::endl;

                    // Redraw palette with new selection (full screen update for UI)
                    drawColorPalette(fb, currentColorIndex);
                    drawClearButton(fb);
                    lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                    frameCount++;
                } else if (isTouchInClearButton(event.x, event.y)) {
                    // Clear button pressed
                    std::cout << "[DRAW] Canvas cleared" << std::endl;
                    clearScreen(fb, COLOR_BLACK);
                    drawColorPalette(fb, currentColorIndex);
                    drawClearButton(fb);
                    lcd.fastCopyFramebuffer(fb, FB_WIDTH * FB_HEIGHT);
                    frameCount++;
                } else {
                    // Start drawing - draw initial dot (PARTIAL UPDATE!)
                    drawCircle(fb, event.x, event.y, 4, COLORS[currentColorIndex].color);

                    // Debug mode: show crosshair at touch position
                    if (debugMode) {
                        drawCrosshair(fb, event.x, event.y, COLOR_CYAN, 15);
                    }

                    // Transform display coords to framebuffer coords for dirty region
                    int fbX, fbY;
                    displayToFramebuffer(event.x, event.y, fbX, fbY);

                    // Update only the region around the dot (larger for crosshair if debug mode)
                    int padding = debugMode ? 20 : 8;
                    int updateX = std::max(0, fbX - padding);
                    int updateY = std::max(0, fbY - padding);
                    int updateW = std::min(padding * 2, FB_WIDTH - updateX);
                    int updateH = std::min(padding * 2, FB_HEIGHT - updateY);

                    lcd.updateRegionFromBuffer(fb, updateX, updateY, updateW, updateH);
                    frameCount++;
                    lastDrawTime = std::chrono::steady_clock::now();

                    // Print touch coordinates in debug mode
                    if (debugMode) {
                        std::cout << "[TOUCH] Display(" << event.x << ", " << event.y
                                  << ") -> FB(" << fbX << ", " << fbY << ")" << std::endl;
                    }
                }

                lastTouchX = event.x;
                lastTouchY = event.y;
                hasDirtyRegion = false;

            } else if (event.state == TOUCH_HELD) {
                // Continue drawing if outside UI areas
                if (event.y >= 75) {  // Below palette (40) + clear button (25) + margin (10)
                    // Draw line from last position to current
                    if (lastTouchX >= 0 && lastTouchY >= 0) {
                        drawLine(fb, lastTouchX, lastTouchY, event.x, event.y, COLORS[currentColorIndex].color);
                        drawCircle(fb, event.x, event.y, 4, COLORS[currentColorIndex].color);

                        // Track dirty region (bounding box of line)
                        int minX = std::min(lastTouchX, (int)event.x);
                        int maxX = std::max(lastTouchX, (int)event.x);
                        int minY = std::min(lastTouchY, (int)event.y);
                        int maxY = std::max(lastTouchY, (int)event.y);

                        if (!hasDirtyRegion) {
                            dirtyMinX = minX;
                            dirtyMinY = minY;
                            dirtyMaxX = maxX;
                            dirtyMaxY = maxY;
                            hasDirtyRegion = true;
                        } else {
                            dirtyMinX = std::min(dirtyMinX, minX);
                            dirtyMinY = std::min(dirtyMinY, minY);
                            dirtyMaxX = std::max(dirtyMaxX, maxX);
                            dirtyMaxY = std::max(dirtyMaxY, maxY);
                        }
                    }

                    // Throttle display updates to ~30 FPS during drag
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDrawTime).count();

                    if (elapsed >= DRAW_THROTTLE_MS && hasDirtyRegion) {
                        // Transform dirty region to framebuffer coords and add padding
                        int fbMinX, fbMinY, fbMaxX, fbMaxY;
                        displayToFramebuffer(dirtyMinX, dirtyMinY, fbMinX, fbMinY);
                        displayToFramebuffer(dirtyMaxX, dirtyMaxY, fbMaxX, fbMaxY);

                        // Ensure min < max (coordinate transform may swap)
                        if (fbMinX > fbMaxX) std::swap(fbMinX, fbMaxX);
                        if (fbMinY > fbMaxY) std::swap(fbMinY, fbMaxY);

                        // Add padding for brush size
                        int updateX = std::max(0, fbMinX - 8);
                        int updateY = std::max(0, fbMinY - 8);
                        int updateW = std::min(fbMaxX - fbMinX + 16, FB_WIDTH - updateX);
                        int updateH = std::min(fbMaxY - fbMinY + 16, FB_HEIGHT - updateY);

                        lcd.updateRegionFromBuffer(fb, updateX, updateY, updateW, updateH);
                        lastDrawTime = now;
                        frameCount++;
                        hasDirtyRegion = false;
                    }
                }

                lastTouchX = event.x;
                lastTouchY = event.y;

            } else if (event.state == TOUCH_RELEASED) {
                // Reset last position
                lastTouchX = -1;
                lastTouchY = -1;
                hasDirtyRegion = false;
            }
        }

        // Calculate and display FPS every second
        auto now = std::chrono::steady_clock::now();
        auto elapsedFPS = std::chrono::duration_cast<std::chrono::seconds>(now - lastFPSTime).count();
        if (elapsedFPS >= 1) {
            currentFPS = frameCount;
            frameCount = 0;
            lastFPSTime = now;
            std::cout << "[DRAW] FPS: " << currentFPS << std::endl;
        }

        usleep(10000);  // 10ms polling
    }

    std::cout << "\n[DRAW] Exiting..." << std::endl;

    // Cleanup
    delete[] fb;
    return 0;
}
