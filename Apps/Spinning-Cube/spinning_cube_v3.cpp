#include <iostream>
#include <cmath>
#include <chrono>
#include <thread>
#include <signal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define LCD_WIDTH  320
#define LCD_HEIGHT 480
#define FB_V3_SHM_NAME_0     "/ili9486_fb_v3_0"
#define FB_V3_STATS_SHM_NAME "/ili9486_fb_v3_stats"

#include <cstring>
#include <cstdio>
#include <atomic>

// Stats structure (must match driver - fb_ili9486_v3.h FramebufferStats)
struct FramebufferStats {
    std::atomic<uint64_t> totalFrames;
    std::atomic<uint64_t> droppedFrames;
    std::atomic<uint32_t> currentFps;
    std::atomic<uint32_t> dirtyPixelCount;
    std::atomic<uint32_t> avgUpdateTimeUs;
    std::atomic<bool> driverActive;
    std::atomic<bool> frameInProgress;
    std::atomic<uint32_t> activeBufferIndex;
    std::atomic<bool> buffer0Done;
    std::atomic<bool> buffer1Done;
};

#define COLOR_BLACK   0x0000
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F

static const char* PID_FILE = "/tmp/spinning_cube_v3.pid";

static pid_t checkExistingInstance() {
    FILE* f = fopen(PID_FILE, "r");
    if (!f) return 0;
    pid_t pid = 0;
    bool ok = (fscanf(f, "%d", &pid) == 1);
    fclose(f);
    if (ok && pid > 0 && kill(pid, 0) == 0) return pid;
    unlink(PID_FILE);  // stale lock — process is gone
    return 0;
}

static void writePidFile() {
    FILE* f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}

static volatile bool running = true;
static void handleSignal(int) { unlink(PID_FILE); running = false; }

struct Vec3 { float x, y, z; };
struct Vec2 { int x, y; };

uint16_t* mapBuffer(const char* name) {
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) {
        std::cerr << "[CUBE_V3] Failed to open " << name << ". Is fb_main_v3 running?" << std::endl;
        return nullptr;
    }
    void* addr = mmap(nullptr, LCD_WIDTH * LCD_HEIGHT * 2,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (addr == MAP_FAILED) ? nullptr : (uint16_t*)addr;
}

FramebufferStats* mapStats() {
    int fd = shm_open(FB_V3_STATS_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) return nullptr;
    void* addr = mmap(nullptr, sizeof(FramebufferStats),
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (addr == MAP_FAILED) ? nullptr : (FramebufferStats*)addr;
}

inline void putPixel(uint16_t* fb, int x, int y, uint16_t color) {
    if ((unsigned)x < LCD_WIDTH && (unsigned)y < LCD_HEIGHT)
        fb[y * LCD_WIDTH + x] = color;
}

void drawLine(uint16_t* fb, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        putPixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

Vec2 project(const Vec3& v, float scale = 3.0f) {
    return { (int)(v.x * scale + LCD_WIDTH  * 0.5f),
             (int)(v.y * scale + LCD_HEIGHT * 0.5f) };
}

int main() {
    signal(SIGINT,  handleSignal);
    signal(SIGTERM, handleSignal);

    pid_t existing = checkExistingInstance();
    if (existing) {
        std::cerr << "[CUBE_V3] Already running (PID " << existing << "). Kill it first:  kill " << existing << std::endl;
        return 1;
    }
    writePidFile();

    std::cout << R"(
╔═══════════════════════════════════════════════╗
║   Spinning Cubes Demo - V3 Edition           ║
║   Not A Bug — It's A Feature Edition         ║
╚═══════════════════════════════════════════════╝
)" << std::endl;

    std::cout << "[CUBE_V3] Connecting to framebuffer driver..." << std::endl;

    uint16_t* fb = mapBuffer(FB_V3_SHM_NAME_0);
    FramebufferStats* stats = mapStats();

    if (!fb || !stats) {
        std::cerr << "[CUBE_V3] Failed to connect. Start fb_main_v3 first!" << std::endl;
        if (fb)    munmap(fb,    LCD_WIDTH * LCD_HEIGHT * 2);
        if (stats) munmap(stats, sizeof(FramebufferStats));
        return 1;
    }

    // Lock to buffer 0 for the lifetime of this app.
    // The dirty detection model requires a stable active buffer — switching buffers
    // mid-stream causes the driver to compare against the wrong previousFrame and
    // leaves ghost content on the display from the prior buffer.
    stats->activeBufferIndex.store(0, std::memory_order_seq_cst);

    // Clear buffer 0 and let the driver sync its previousFrame to all-black.
    memset(fb, 0, LCD_WIDTH * LCD_HEIGHT * 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "[CUBE_V3] Connected! Rendering cubes..." << std::endl;
    std::cout << "  ✓ Two cubes, opposite spin — not a bug, it's a feature" << std::endl;
    std::cout << "  ✓ Single frameInProgress window covers both cubes atomically" << std::endl;
    std::cout << "  ✓ Erase-then-draw: dirty region stays tight around both cubes" << std::endl;
    std::cout << "[CUBE_V3] Press Ctrl+C to exit." << std::endl;

    const float s = 20.0f;
    Vec3 cubeVerts[8] = {
        {-s,-s,-s}, { s,-s,-s}, { s, s,-s}, {-s, s,-s},
        {-s,-s, s}, { s,-s, s}, { s, s, s}, {-s, s, s}
    };
    int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };

    // Cube 1 — RGB, normal rotation
    uint16_t edgeColors1[12] = {
        COLOR_RED,   COLOR_RED,   COLOR_RED,   COLOR_RED,
        COLOR_GREEN, COLOR_GREEN, COLOR_GREEN, COLOR_GREEN,
        COLOR_BLUE,  COLOR_BLUE,  COLOR_BLUE,  COLOR_BLUE
    };
    float angX1 = 0.0f, angY1 = 0.0f, angZ1 = 0.0f;
    const float wX1 = 0.8f, wY1 = 1.1f, wZ1 = 0.6f;

    // Cube 2 — CMY, all axes flipped and shuffled so it counter-spins
    uint16_t edgeColors2[12] = {
        COLOR_YELLOW,  COLOR_YELLOW,  COLOR_YELLOW,  COLOR_YELLOW,
        COLOR_CYAN,    COLOR_CYAN,    COLOR_CYAN,    COLOR_CYAN,
        COLOR_MAGENTA, COLOR_MAGENTA, COLOR_MAGENTA, COLOR_MAGENTA
    };
    float angX2 = 0.0f, angY2 = 0.0f, angZ2 = 0.0f;
    const float wX2 = -1.1f, wY2 = 0.6f, wZ2 = -0.8f;

    Vec2 prevProjected1[8] = {};
    Vec2 prevProjected2[8] = {};
    bool firstFrame = true;

    auto lastFrame = std::chrono::high_resolution_clock::now();

    while (running) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;

        // Advance cube 1 angles
        angX1 += wX1 * dt;  angY1 += wY1 * dt;  angZ1 += wZ1 * dt;
        // Advance cube 2 angles (counter-spinning)
        angX2 += wX2 * dt;  angY2 += wY2 * dt;  angZ2 += wZ2 * dt;

        // Project cube 1
        Vec2 projected1[8];
        {
            float sx = std::sin(angX1), cx = std::cos(angX1);
            float sy = std::sin(angY1), cy = std::cos(angY1);
            float sz = std::sin(angZ1), cz = std::cos(angZ1);
            for (int i = 0; i < 8; i++) {
                Vec3 v = cubeVerts[i];
                float y1 = v.y*cx - v.z*sx,  z1 = v.y*sx + v.z*cx;  v.y = y1; v.z = z1;
                float x2 = v.x*cy + v.z*sy,  z2 =-v.x*sy + v.z*cy;  v.x = x2; v.z = z2;
                float x3 = v.x*cz - v.y*sz,  y3 = v.x*sz + v.y*cz;  v.x = x3; v.y = y3;
                projected1[i] = project(v);
            }
        }

        // Project cube 2
        Vec2 projected2[8];
        {
            float sx = std::sin(angX2), cx = std::cos(angX2);
            float sy = std::sin(angY2), cy = std::cos(angY2);
            float sz = std::sin(angZ2), cz = std::cos(angZ2);
            for (int i = 0; i < 8; i++) {
                Vec3 v = cubeVerts[i];
                float y1 = v.y*cx - v.z*sx,  z1 = v.y*sx + v.z*cx;  v.y = y1; v.z = z1;
                float x2 = v.x*cy + v.z*sy,  z2 =-v.x*sy + v.z*cy;  v.x = x2; v.z = z2;
                float x3 = v.x*cz - v.y*sz,  y3 = v.x*sz + v.y*cz;  v.x = x3; v.y = y3;
                projected2[i] = project(v);
            }
        }

        // Single frameInProgress window covers both cubes — driver sees one atomic frame
        stats->frameInProgress.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!firstFrame) {
            // Erase both cubes' previous edges
            for (int i = 0; i < 12; i++) {
                drawLine(fb,
                         prevProjected1[edges[i][0]].x, prevProjected1[edges[i][0]].y,
                         prevProjected1[edges[i][1]].x, prevProjected1[edges[i][1]].y,
                         COLOR_BLACK);
                drawLine(fb,
                         prevProjected2[edges[i][0]].x, prevProjected2[edges[i][0]].y,
                         prevProjected2[edges[i][1]].x, prevProjected2[edges[i][1]].y,
                         COLOR_BLACK);
            }
        }

        // Draw both cubes' new edges
        for (int i = 0; i < 12; i++) {
            drawLine(fb,
                     projected1[edges[i][0]].x, projected1[edges[i][0]].y,
                     projected1[edges[i][1]].x, projected1[edges[i][1]].y,
                     edgeColors1[i]);
            drawLine(fb,
                     projected2[edges[i][0]].x, projected2[edges[i][0]].y,
                     projected2[edges[i][1]].x, projected2[edges[i][1]].y,
                     edgeColors2[i]);
        }

        std::atomic_thread_fence(std::memory_order_seq_cst);
        stats->frameInProgress.store(false, std::memory_order_seq_cst);

        for (int i = 0; i < 8; i++) { prevProjected1[i] = projected1[i]; prevProjected2[i] = projected2[i]; }
        firstFrame = false;

        // ~60fps cap
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    munmap(fb,    LCD_WIDTH * LCD_HEIGHT * 2);
    munmap(stats, sizeof(FramebufferStats));
    unlink(PID_FILE);
    return 0;
}
