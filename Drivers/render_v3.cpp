/*
 * render_v3.cpp — Software Render Engine implementation
 *
 * Landscape mode (orientation=0 in settings.conf, 480×320 physical display):
 *   The ILI9486 swaps its addressing so the framebuffer memory row index
 *   corresponds to the physical horizontal axis.  In code terms:
 *
 *     fb_[phys_x * LCD_W + phys_y]
 *
 *   where phys_x ∈ [0, 479] is the physical column (horizontal) and
 *         phys_y ∈ [0, 319] is the physical row (vertical).
 *
 *   This is a pure transposition of the 320×480 portrait layout.
 *   Every "physical column" is a contiguous run of 320 uint16_t values,
 *   so wall-strip writes in the raycaster are cache-perfect in landscape.
 *
 * Portrait mode (orientation=90, 320×480 physical display):
 *   Standard layout: fb_[phys_y * LCD_W + phys_x].
 */

#include "render_v3.h"

#include <cmath>
#include <cstring>
#include <atomic>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

// ── Lifecycle ─────────────────────────────────────────────────────────────────

bool RenderV3::init() {
    int fd = shm_open(RDR_FB_SHM, O_RDWR, 0666);
    if (fd < 0) return false;
    void* p = mmap(nullptr, LCD_W * LCD_H * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    fb_ = (uint16_t*)p;

    fd = shm_open(RDR_STATS_SHM, O_RDWR, 0666);
    if (fd < 0) { munmap(fb_, LCD_W * LCD_H * 2); fb_ = nullptr; return false; }
    p = mmap(nullptr, sizeof(FramebufferStats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { munmap(fb_, LCD_W * LCD_H * 2); fb_ = nullptr; return false; }
    stats_ = (FramebufferStats*)p;

    stats_->activeBufferIndex.store(0, std::memory_order_seq_cst);
    return true;
}

void RenderV3::shutdown() {
    if (fb_)    { munmap(fb_,    LCD_W * LCD_H * 2);        fb_    = nullptr; }
    if (stats_) { munmap(stats_, sizeof(FramebufferStats));  stats_ = nullptr; }
}

// ── Frame synchronization ─────────────────────────────────────────────────────

void RenderV3::beginFrame() {
    stats_->frameInProgress.store(true, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void RenderV3::endFrame() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    stats_->frameInProgress.store(false, std::memory_order_seq_cst);
}

// ── Internal fb write helpers ─────────────────────────────────────────────────
//
// landscape_ = false (portrait):  fb_[py * LCD_W + px]
// landscape_ = true  (landscape): fb_[px * LCD_W + py]   ← transposed

// ── 2-D Primitives ────────────────────────────────────────────────────────────

void RenderV3::clear(uint16_t color) {
    if (color == 0x0000) {
        memset(fb_, 0, (size_t)LCD_W * LCD_H * 2);
    } else {
        uint16_t* p = fb_, *end = p + LCD_W * LCD_H;
        while (p < end) *p++ = color;
    }
}

void RenderV3::putPixel(int x, int y, uint16_t color) {
    if ((unsigned)x >= (unsigned)physW() || (unsigned)y >= (unsigned)physH()) return;
    fb_[landscape_ ? x * LCD_W + y : y * LCD_W + x] = color;
}

// fillRect — physical (x,y,w,h).
// Both modes iterate over columns first so the inner loop is always contiguous.
void RenderV3::fillRect(int x, int y, int w, int h, uint16_t color) {
    int PW = physW(), PH = physH();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PW) w = PW - x;
    if (y + h > PH) h = PH - y;
    if (w <= 0 || h <= 0) return;

    if (landscape_) {
        // fb_[col * LCD_W + row] — inner loop over row = contiguous
        for (int col = x; col < x + w; col++) {
            uint16_t* p = fb_ + col * LCD_W + y;
            for (int row = 0; row < h; row++) p[row] = color;
        }
    } else {
        // fb_[row * LCD_W + col] — inner loop over col = contiguous
        for (int row = y; row < y + h; row++) {
            uint16_t* p = fb_ + row * LCD_W + x;
            for (int col = 0; col < w; col++) p[col] = color;
        }
    }
}

// vline — physical vertical strip (constant x, varying y): always contiguous in landscape.
void RenderV3::vline(int x, int y0, int y1, uint16_t color) {
    int PH = physH();
    if ((unsigned)x >= (unsigned)physW()) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= PH) y1 = PH - 1;
    if (y0 > y1) return;
    if (landscape_) {
        uint16_t* p = fb_ + x * LCD_W + y0;
        for (int y = y0; y <= y1; y++) *p++ = color;   // stride 1, contiguous ✓
    } else {
        for (int y = y0; y <= y1; y++) fb_[y * LCD_W + x] = color;
    }
}

// hline — physical horizontal strip (constant y, varying x): always contiguous in portrait.
void RenderV3::hline(int y, int x0, int x1, uint16_t color) {
    int PW = physW();
    if ((unsigned)y >= (unsigned)physH()) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= PW) x1 = PW - 1;
    if (x0 > x1) return;
    if (landscape_) {
        for (int x = x0; x <= x1; x++) fb_[x * LCD_W + y] = color;
    } else {
        uint16_t* p = fb_ + y * LCD_W + x0;
        for (int x = x0; x <= x1; x++) *p++ = color;   // stride 1, contiguous ✓
    }
}

void RenderV3::drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = -abs(y1-y0), sy = y0<y1?1:-1;
    int err = dx + dy;
    for (;;) {
        putPixel(x0, y0, color);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ── Color utilities ───────────────────────────────────────────────────────────

uint16_t RenderV3::rgb(int r, int g, int b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint16_t RenderV3::shade(uint16_t c, float dist, float maxDist) {
    float t = 1.0f - dist / maxDist;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (uint16_t)((((c>>11)&0x1F)*(int)(t*31)/31 << 11) |
                      (((c>> 5)&0x3F)*(int)(t*63)/63 <<  5) |
                      (( c     &0x1F)*(int)(t*31)/31));
}

// ── Raycaster ─────────────────────────────────────────────────────────────────

static inline bool rayHit(const uint8_t* map, int mapW, int mapH, int x, int y) {
    if ((unsigned)x >= (unsigned)mapW || (unsigned)y >= (unsigned)mapH) return true;
    return map[y * mapW + x] != 0;
}

void RenderV3::raycaster(float px, float py, float angle,
                         int viewX, int viewY, int viewW, int viewH,
                         const uint8_t* map, int mapW, int mapH,
                         uint16_t wallNS, uint16_t wallEW,
                         uint16_t ceilColor, uint16_t floorColor,
                         uint16_t doorColor,
                         float fov, float maxDist)
{
    const float dirX   =  cosf(angle);
    const float dirY   =  sinf(angle);
    const float planeX = -dirY * fov;
    const float planeY =  dirX * fov;
    const int   halfH  = viewH / 2;

    // Solid ceiling / floor — no gradient.
    // Gradients and distance-shading produce hundreds of unique RGB565 values
    // per frame, causing signal-integrity corruption at 80 MHz SPI.
    // Flat solid colours keep the SPI data stream clean.

    // Initialise dirty cache on first call (castY1_ sentinel = -1)
    if (!castInit_) {
        memset(castY0_, 0x00, sizeof(castY0_));
        memset(castY1_, 0xFF, sizeof(castY1_));   // 0xFFFF = -1 as int16_t
        castInit_ = true;
    }

    // ── Per-column DDA ────────────────────────────────────────────────────────
    for (int col = 0; col < viewW; col++) {
        // Negate camX to correct the left-right mirror introduced by the
        // ILI9486 landscape MADCTL transposition (MX=1 reverses column order).
        float camX = 1.0f - 2.0f * col / viewW;
        float rdX  = dirX + planeX * camX;
        float rdY  = dirY + planeY * camX;

        int   mx   = (int)px,   my = (int)py;
        float ddX  = (rdX == 0.0f) ? 1e30f : fabsf(1.0f / rdX);
        float ddY  = (rdY == 0.0f) ? 1e30f : fabsf(1.0f / rdY);
        float sdX, sdY;
        int   stepX, stepY, side = 0;

        if (rdX < 0.0f) { stepX = -1; sdX = (px - mx)        * ddX; }
        else            { stepX =  1; sdX = (mx + 1.0f - px) * ddX; }
        if (rdY < 0.0f) { stepY = -1; sdY = (py - my)        * ddY; }
        else            { stepY =  1; sdY = (my + 1.0f - py) * ddY; }

        for (int step = 0, maxS = mapW + mapH; step < maxS; step++) {
            if (sdX < sdY) { sdX += ddX; mx += stepX; side = 0; }
            else           { sdY += ddY; my += stepY; side = 1; }
            if (rayHit(map, mapW, mapH, mx, my)) break;
        }

        float dist = (side == 0)
            ? ((float)mx - px + (1.0f - (float)stepX) * 0.5f) / rdX
            : ((float)my - py + (1.0f - (float)stepY) * 0.5f) / rdY;
        if (dist < 0.01f) dist = 0.01f;

        int lineH = (int)((float)viewH / dist);
        int y0    = halfH - lineH / 2;
        int y1    = halfH + lineH / 2;
        if (y0 < 0)       y0 = 0;
        if (y1 >= viewH)  y1 = viewH - 1;

        // Flat solid wall/door colour — no distance shading.
        // shade() produces unique intermediate RGB565 values that cause SPI
        // corruption at 80 MHz; using a single solid colour per surface keeps
        // the bitstream clean.
        uint8_t  cellVal = map[my * mapW + mx];
        uint16_t wc      = (cellVal == 2) ? doorColor
                         : (side == 0 ? wallEW : wallNS);

        // ── Dirty-aware column write ──────────────────────────────────────────
        // On first draw (pY1 < 0): paint ceiling + wall + floor in full.
        // Subsequently: only restore solid ceiling/floor where wall boundary
        // moved.  Wall strip always redrawn (wc may differ NS vs EW side).
        // Pixels that didn't change are left untouched → driver sees a much
        // smaller dirty region → less SPI data every frame.
        const int    pcol = viewX + col;
        const int16_t pY0 = castY0_[pcol];
        const int16_t pY1 = castY1_[pcol];

        if (landscape_) {
            uint16_t* base = fb_ + pcol * LCD_W + viewY;
            if (pY1 < 0) {
                // First frame: write every pixel in the column
                for (int row = 0;    row < y0;    row++) base[row] = ceilColor;
                for (int row = y0;   row <= y1;   row++) base[row] = wc;
                for (int row = y1+1; row < viewH; row++) base[row] = floorColor;
            } else {
                // Restore solid ceiling only where wall retreated downward
                for (int row = pY0;  row < y0;    row++) base[row] = ceilColor;
                // Restore solid floor only where wall retreated upward
                for (int row = y1+1; row <= pY1;  row++) base[row] = floorColor;
                // Wall strip always redrawn (NS/EW side can flip each frame)
                for (int row = y0;   row <= y1;   row++) base[row] = wc;
            }
        } else {
            if (pY1 < 0) {
                for (int row = 0;    row < y0;    row++) fb_[(viewY+row)*LCD_W + pcol] = ceilColor;
                for (int row = y0;   row <= y1;   row++) fb_[(viewY+row)*LCD_W + pcol] = wc;
                for (int row = y1+1; row < viewH; row++) fb_[(viewY+row)*LCD_W + pcol] = floorColor;
            } else {
                for (int row = pY0;  row < y0;    row++) fb_[(viewY+row)*LCD_W + pcol] = ceilColor;
                for (int row = y1+1; row <= pY1;  row++) fb_[(viewY+row)*LCD_W + pcol] = floorColor;
                for (int row = y0;   row <= y1;   row++) fb_[(viewY+row)*LCD_W + pcol] = wc;
            }
        }

        castY0_[pcol] = (int16_t)y0;
        castY1_[pcol] = (int16_t)y1;
    }
}
