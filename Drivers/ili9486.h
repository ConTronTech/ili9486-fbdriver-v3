#ifndef ILI9486_H
#define ILI9486_H

#include <cstdint>
#include <string>
#include <vector>

class ILI9486 {
public:
    ILI9486(const std::string& spiDevice, int dcPin, int rstPin, uint32_t spiSpeed = 32000000);
    ~ILI9486();

    bool init();
    void drawPixel(uint16_t x, uint16_t y, uint16_t color);
    void fastFillScreen(uint16_t color);
    void copyFramebuffer(const uint16_t* buffer, size_t bufferSize);
    void fastCopyFramebuffer(const uint16_t* buffer, size_t bufferSize);

    // Performance optimizations
    void enableDMA(bool enable = true);
    void setDoubleBuffering(bool enable = true);
    void updateRegion(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
    void updateRegionFromBuffer(const uint16_t* buffer, uint16_t x, uint16_t y, uint16_t width, uint16_t height);
    void swapBuffers();
    void setRefreshRate(int fps);

    // Display orientation and mirroring
    void setOrientation(int degrees);  // 0, 90, 180, 270
    void setMirrorX(bool mirror);
    void setMirrorY(bool mirror);

    // Accessor methods for framebuffer driver
    int getDCPin() const { return dcPin; }
    int getSPIFd() const { return spi_fd; }
    uint32_t getSPISpeed() const { return spiSpeed; }
    void setSPISpeed(uint32_t hz) { spiSpeed = hz; }  // Takes effect on next transfer
    void setAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    
    // Add more drawing methods as needed

private:
    int spi_fd;
    int dcPin, rstPin;
    std::string spiDevice;
    uint32_t spiSpeed;

    // Display settings
    int orientation;      // 0, 90, 180, 270 degrees
    bool mirrorX;
    bool mirrorY;

    // Performance optimization variables
    bool dmaEnabled;
    bool doubleBuffering;
    std::vector<uint16_t> backBuffer;
    std::vector<uint16_t> frontBuffer;
    int refreshRate;
    
    // Dirty rectangle tracking
    struct DirtyRect {
        uint16_t x, y, width, height;
        bool dirty;
    } dirtyRect;

    // pigpio mode tracking
    int pigpioHandle;      // Handle for daemon mode, or version for standalone
    bool isDaemonMode;     // true = using daemon, false = standalone

    bool openSPI();
    void closeSPI();
    void reset();
    void writeCommand(uint8_t cmd);
    void writeData(uint8_t data);
    void writeData16(uint16_t data);

    // Display control
    void applyMADCTL();  // Apply Memory Access Control settings

    // Performance methods
    void initBuffers();
    void markDirty(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
    void clearDirty();
    void updateDirtyRegion();
};

#endif // ILI9486_H 