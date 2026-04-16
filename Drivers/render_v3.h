/*
 * render_v3.h — Software Render Engine for ILI9486 V3
 *
 * Wraps the shared-memory framebuffer and provides:
 *   • Frame lifecycle  (beginFrame / endFrame)
 *   • 2-D primitives   (putPixel, fillRect, drawLine, vline, hline)
 *   • Color helpers    (rgb, shade)
 *   • High-level DDA raycaster with viewport support + gradient sky/floor
 *
 * Apps include ONLY this header — never define FramebufferStats themselves.
 *
 * Usage:
 *   RenderV3 rdr;
 *   rdr.setLandscape(true);   // call BEFORE init() — matches orientation in settings.conf
 *   if (!rdr.init()) return 1;
 *
 *   rdr.beginFrame();
 *   rdr.clear();
 *   // viewport uses PHYSICAL coordinates: rdr.physW() × rdr.physH()
 *   rdr.raycaster(px, py, angle, 0, 0, rdr.physW(), 240, map, 128, 128);
 *   rdr.endFrame();
 *
 *   rdr.shutdown();
 */

#pragma once
#include <cstdint>
#include <atomic>

// ── Shared-memory names ───────────────────────────────────────────────────────
static constexpr const char* RDR_FB_SHM    = "/ili9486_fb_v3_0";
static constexpr const char* RDR_STATS_SHM = "/ili9486_fb_v3_stats";

// ── FramebufferStats — must mirror fb_ili9486_v3.h exactly ───────────────────
struct FramebufferStats {
    std::atomic<uint64_t> totalFrames;
    std::atomic<uint64_t> droppedFrames;
    std::atomic<uint32_t> currentFps;
    std::atomic<uint32_t> dirtyPixelCount;
    std::atomic<uint32_t> avgUpdateTimeUs;
    std::atomic<bool>     driverActive;
    std::atomic<bool>     frameInProgress;   // set by app during draw
    std::atomic<uint32_t> activeBufferIndex; // 0 or 1
    std::atomic<bool>     buffer0Done;
    std::atomic<bool>     buffer1Done;
    std::atomic<uint32_t> cpuTempMilliC;
    std::atomic<uint32_t> thermalState;
    std::atomic<uint32_t> requestedSpiSpeedHz;
};

// ── RenderV3 ──────────────────────────────────────────────────────────────────
class RenderV3 {
public:
    // Physical framebuffer dimensions (portrait memory layout)
    static constexpr int LCD_W = 320;
    static constexpr int LCD_H = 480;

    // ── Orientation ───────────────────────────────────────────────────────────
    // Call setLandscape(true) when settings.conf orientation=0 (the 480×320 mode).
    // All drawing primitives and raycaster() then use PHYSICAL coordinates where
    // x ∈ [0, physW()-1] is the horizontal axis and y ∈ [0, physH()-1] is vertical.
    //
    // The ILI9486 landscape mode transposes the framebuffer so that app_y maps to
    // the physical horizontal axis.  The render engine handles this internally;
    // callers just use natural (x, y) physical-screen coordinates.
    void setLandscape(bool landscape) { landscape_ = landscape; }
    int  physW() const { return landscape_ ? LCD_H : LCD_W; }  // 480 landscape / 320 portrait
    int  physH() const { return landscape_ ? LCD_W : LCD_H; }  // 320 landscape / 480 portrait

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    // init() opens shared memory and locks to buffer 0.
    // Returns false if fb_main_v3 is not running.
    bool init();
    void shutdown();

    // ── Frame synchronization ─────────────────────────────────────────────────
    // beginFrame / endFrame bracket every draw call.
    // The driver pauses dirty detection while frameInProgress is set.
    void beginFrame();
    void endFrame();

    // ── 2-D primitives ────────────────────────────────────────────────────────
    void clear(uint16_t color = 0x0000);
    void putPixel(int x, int y, uint16_t color);
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color);
    void vline(int x, int y0, int y1, uint16_t color);   // vertical strip
    void hline(int y, int x0, int x1, uint16_t color);   // horizontal strip

    // ── Color utilities ───────────────────────────────────────────────────────
    // rgb()   — convert 8-bit R,G,B to RGB565
    // shade() — darken a colour by distance (full-bright at 0, black at maxDist)
    static uint16_t rgb(int r, int g, int b);
    static uint16_t shade(uint16_t color, float dist, float maxDist = 16.0f);

    // ── High-level renderers ──────────────────────────────────────────────────
    //
    // raycaster() — classic DDA raycaster.
    //
    //   px, py, angle   — player world position and facing angle (radians)
    //   viewX/Y/W/H     — viewport rectangle inside the framebuffer
    //   map             — flat uint8_t array [mapH * mapW], 1=wall 0=open
    //   mapW, mapH      — map dimensions
    //   wallNS          — base colour for N/S-facing walls (brighter)
    //   wallEW          — base colour for E/W-facing walls (dimmer, free shading)
    //   ceilColor       — ceiling base colour (gradient fades to black at top)
    //   floorColor      — floor base colour (gradient fades to black at bottom)
    //   fov             — camera plane half-length (0.66 ≈ 66° horizontal FOV)
    //   maxDist         — distance at which walls fade to black
    void raycaster(float px, float py, float angle,
                   int viewX, int viewY, int viewW, int viewH,
                   const uint8_t* map, int mapW, int mapH,
                   uint16_t wallNS    = 0x07C0,   // bright green
                   uint16_t wallEW    = 0x03E0,   // darker green
                   uint16_t ceilColor = 0x10AA,   // dark navy  rgb(16,20,80)
                   uint16_t floorColor= 0x29E5,   // dark moss  rgb(40,60,40)
                   uint16_t doorColor = 0xB322,   // warm wood  rgb(180,100,20)
                   float fov = 0.66f, float maxDist = 16.0f);

    // ── Raw access ────────────────────────────────────────────────────────────
    uint16_t*         framebuffer() const { return fb_; }
    FramebufferStats* stats()       const { return stats_; }

    // Invalidate the dirty-aware raycaster cache (call when changing scenes/maps)
    void invalidateRaycasterCache() { castInit_ = false; }

    // Request a runtime SPI clock change through shared memory.
    // The driver picks this up within one frame cycle (~33 ms at 30 fps).
    // hz = 0 asks the driver to restore its configured default speed.
    // Thermal throttle still applies on top (HOT halves it, CRITICAL quarters it).
    void requestSpiSpeed(uint32_t hz) {
        if (stats_) stats_->requestedSpiSpeedHz.store(hz, std::memory_order_relaxed);
    }

private:
    uint16_t*         fb_        = nullptr;
    FramebufferStats* stats_     = nullptr;
    bool              landscape_ = false;

    // ── Dirty-aware raycaster cache ───────────────────────────────────────────
    // Tracks per-physical-column wall-strip bounds so we only write ceiling/floor
    // pixels that actually changed, keeping the driver's dirty region minimal.
    // castY1_ == -1 means "not yet drawn" → forces a full column draw.
    int16_t castY0_[LCD_H] = {};
    int16_t castY1_[LCD_H] = {};   // -1 sentinel set in first raycaster call
    bool    castInit_ = false;
};
