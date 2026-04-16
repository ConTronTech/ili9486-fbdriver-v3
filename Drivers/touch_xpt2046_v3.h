#ifndef TOUCH_XPT2046_V3_H
#define TOUCH_XPT2046_V3_H

#include <atomic>
#include <cstdint>
#include <string>

// Touch event states
enum TouchState {
    TOUCH_RELEASED = 0,
    TOUCH_PRESSED = 1,
    TOUCH_HELD = 2
};

// Touch event structure (shared memory)
struct TouchEvent {
    int16_t x;              // X coordinate (screen space)
    int16_t y;              // Y coordinate (screen space)
    uint16_t pressure;      // Pressure/Z value (0-4095)
    TouchState state;       // Touch state
    uint64_t timestamp;     // Timestamp in microseconds
    bool valid;             // Is this event valid?
};

// Touch statistics (shared memory)
struct TouchStats {
    std::atomic<uint64_t> totalEvents;       // Total touch events processed
    std::atomic<uint32_t> currentPressure;   // Current pressure reading
    std::atomic<bool> touchActive;           // Is touch currently active?
    std::atomic<uint32_t> pollRate;          // Current polling rate (Hz)
    std::atomic<bool> driverActive;          // Is driver running?
    std::atomic<uint32_t> eventQueueHead;    // Event queue head index
    std::atomic<uint32_t> eventQueueTail;    // Event queue tail index
};

// Calibration point structure for 9-point calibration
struct CalibrationPoint {
    int nativeX, nativeY;  // Native touch coordinates (320x480 portrait)
    int rawX, rawY;        // Raw ADC values (0-4095)
};

// Touch configuration
struct TouchConfig {
    std::string spiDevice;
    uint32_t spiSpeed;

    // Native touch sensor dimensions (physical orientation before rotation)
    int nativeWidth;           // Native sensor width (e.g., 320 for portrait sensor)
    int nativeHeight;          // Native sensor height (e.g., 480 for portrait sensor)

    // Display dimensions (after orientation transform)
    int displayWidth;          // Display width at current orientation
    int displayHeight;         // Display height at current orientation

    // Display orientation (0/90/180/270 degrees clockwise)
    int orientation;

    // 9-point calibration data (3x3 grid)
    // Grid layout:  0  1  2
    //               3  4  5
    //               6  7  8
    int numCalPoints;              // Number of calibration points (9 for grid)
    CalibrationPoint calPoints[9]; // Calibration grid points

    // Legacy calibration values (for backward compatibility or fallback)
    int calXMin, calXMax;      // Maps raw X to nativeWidth
    int calYMin, calYMax;      // Maps raw Y to nativeHeight

    uint16_t pressureThreshold;
    int pollRateHz;
};

// XPT2046 Touch Controller Class
class TouchXPT2046 {
public:
    TouchXPT2046(const TouchConfig& config);
    ~TouchXPT2046();
    
    // Initialize SPI and touch controller
    bool init();
    
    // Read touch state and position
    bool readTouch(TouchEvent& event);
    
    // Check if touch is currently active
    bool isTouched();
    
    // Get raw touch coordinates (uncalibrated ADC values)
    void getRawPosition(int& x, int& y, int& z);
    
    // Calibrate raw ADC values to native sensor coordinates
    void calibratePosition(int rawX, int rawY, int& nativeX, int& nativeY);
    
    // Transform native sensor coords to display coords based on orientation
    void transformForDisplay(int nativeX, int nativeY, int& displayX, int& displayY);

private:
    TouchConfig config_;
    int spiFd_;
    TouchState lastState_;
    int lastX_, lastY_;
    
    // SPI communication
    bool spiTransfer(const uint8_t* tx, uint8_t* rx, size_t len);
    
    // Read raw value from touch controller
    int readRaw(uint8_t command);
    
    
    // Read averaged samples for better accuracy
    bool readAveraged(int& x, int& y, int& z, int samples = 7);
};

#endif // TOUCH_XPT2046_V3_H
