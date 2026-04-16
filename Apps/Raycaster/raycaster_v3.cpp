/*
 * raycaster_v3.cpp — Infinite Maze Raycaster (uses RenderV3 engine)
 *
 * This app owns: maze generation, player state, input loop.
 * All rendering is delegated to RenderV3.
 *
 * Controls:
 *   W / S  — move forward / backward
 *   A / D  — turn left / right
 *   Q      — quit
 */

#include "../../Drivers/render_v3.h"

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <signal.h>
#include <unistd.h>
#include <termios.h>

// ── Map ───────────────────────────────────────────────────────────────────────
static constexpr int MAP_W = 128;
static constexpr int MAP_H = 128;

static uint8_t gMap [MAP_H][MAP_W];   // 0=open  1=wall  2=closed door
static uint8_t gDoors[MAP_H][MAP_W];  // 1=door cell exists here (open or closed)

static void generateMaze(uint32_t seed) {
    memset(gMap, 1, sizeof(gMap));

    const int CW = (MAP_W - 1) / 2;   // 63 cells wide
    const int CH = (MAP_H - 1) / 2;   // 63 cells tall

    static bool visited[63 * 63];
    static int  stkX[63 * 63];
    static int  stkY[63 * 63];
    memset(visited, 0, sizeof(visited));

    auto rng = [](uint32_t& s) -> uint32_t { return s = s * 1664525u + 1013904223u; };

    const int DX[4] = { 0, 0, 1, -1 };
    const int DY[4] = { 1,-1, 0,  0 };

    int cx0 = CW / 2, cy0 = CH / 2;
    visited[cy0 * CW + cx0] = true;
    gMap[cy0*2+1][cx0*2+1] = 0;

    int top = 0;
    stkX[0] = cx0;
    stkY[0] = cy0;

    while (top >= 0) {
        int cx = stkX[top], cy = stkY[top];

        int nbr[4], nc = 0;
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if ((unsigned)nx < (unsigned)CW && (unsigned)ny < (unsigned)CH
                    && !visited[ny*CW+nx])
                nbr[nc++] = d;
        }

        if (nc == 0) { --top; continue; }

        int d  = nbr[rng(seed) % nc];
        int nx = cx + DX[d], ny = cy + DY[d];

        gMap[cy*2+1 + DY[d]][cx*2+1 + DX[d]] = 0;
        gMap[ny*2+1][nx*2+1] = 0;
        visited[ny*CW+nx] = true;
        stkX[++top] = nx;
        stkY[top]   = ny;
    }
}

// ── Door placement ────────────────────────────────────────────────────────────
// Doors are placed on passage cells — cells where exactly one coordinate is
// even (they sit between two cell centres, not at corners or cell centres).
// ~1-in-6 passages get a door.
static void placeDoors(uint32_t seed) {
    memset(gDoors, 0, sizeof(gDoors));
    auto rng = [](uint32_t& s) -> uint32_t { return s = s * 1664525u + 1013904223u; };
    for (int y = 1; y < MAP_H - 1; y++) {
        for (int x = 1; x < MAP_W - 1; x++) {
            if (gMap[y][x] != 0) continue;           // must be an open passage
            bool xEven = (x % 2 == 0);
            bool yEven = (y % 2 == 0);
            if (xEven == yEven) continue;             // skip cell centres & wall corners
            if ((rng(seed) % 6) != 0) continue;      // ~16 % of passages
            gMap[y][x]  = 2;                          // closed door
            gDoors[y][x] = 1;
        }
    }
}

// ── Single-instance guard ─────────────────────────────────────────────────────
static const char*   PID_FILE = "/tmp/raycaster_v3.pid";
static volatile bool running  = true;

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
static void handleSignal(int) { unlink(PID_FILE); running = false; }

// ── Terminal raw-input ────────────────────────────────────────────────────────
static struct termios sOrigTermios;
static void rawOn() {
    tcgetattr(STDIN_FILENO, &sOrigTermios);
    struct termios t = sOrigTermios;
    t.c_lflag &= ~(unsigned)(ICANON | ECHO);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}
static void rawOff() { tcsetattr(STDIN_FILENO, TCSANOW, &sOrigTermios); }

// ── Main ──────────────────────────────────────────────────────────────────────
// Landscape display (orientation=0 in settings.conf): 480 wide × 320 tall.
// Full-screen 3D view — no status bar.
// VIEW_H is set dynamically from rdr.physH() after init.

int main(int argc, char* argv[]) {
    // ── Argument parsing ──────────────────────────────────────────────────────
    uint32_t spiMhz = 80;   // default: clock up to 80 MHz
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mhz") == 0 && i + 1 < argc) {
            uint32_t v = (uint32_t)atoi(argv[++i]);
            if (v == 32 || v == 64 || v == 80) {
                spiMhz = v;
            } else {
                fprintf(stderr, "[RAYCASTER] --mhz must be 32, 64, or 80. Got %u.\n", v);
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: raycaster_v3 [--mhz 32|64|80]\n"
                   "  --mhz N   Request SPI clock speed (default 80)\n");
            return 0;
        }
    }

    signal(SIGINT,  handleSignal);
    signal(SIGTERM, handleSignal);

    pid_t ex = checkExistingInstance();
    if (ex) {
        fprintf(stderr, "[RAYCASTER] Already running (PID %d). Kill it first:  kill %d\n", ex, ex);
        return 1;
    }
    writePidFile();

    printf("\n"
           "╔═══════════════════════════════════════════════╗\n"
           "║   Infinite Maze Raycaster — V3 Edition       ║\n"
           "║   W/S = move   A/D = turn   E = door   Q = quit ║\n"
           "╚═══════════════════════════════════════════════╝\n\n");

    uint32_t seed = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count();
    printf("[RAYCASTER] Generating %d×%d maze (seed 0x%08X)...\n", MAP_W, MAP_H, seed);
    generateMaze(seed);
    placeDoors(seed ^ 0xDEADBEEFu);
    printf("[RAYCASTER] Maze ready.\n");

    RenderV3 rdr;
    rdr.setLandscape(true);   // orientation=0 in settings.conf = 480×320 landscape
    if (!rdr.init()) {
        fprintf(stderr, "[RAYCASTER] fb_main_v3 not running — start it first!\n");
        unlink(PID_FILE);
        return 1;
    }
    rdr.requestSpiSpeed(spiMhz * 1000000u);
    printf("[RAYCASTER] Requested %u MHz SPI clock.\n", spiMhz);

    const int VIEW_H = rdr.physH();   // full 320px in landscape

    rdr.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Maze cells sit at odd map indices: cell (cx0,cy0) = (CW/2, CH/2)
    // where CW = (MAP_W-1)/2 = 63 → cx0 = 31 → map_x = cx0*2+1 = 63
    // Map index 64 (MAP_W/2) is a wall row — player must land on index 63.
    static constexpr int START_MX = ((MAP_W - 1) / 2 / 2) * 2 + 1;  // 63
    static constexpr int START_MY = ((MAP_H - 1) / 2 / 2) * 2 + 1;  // 63

    float px = START_MX + 0.5f;   // 63.5 — centre of the open start cell
    float py = START_MY + 0.5f;   // 63.5

    // Face the first open corridor so W moves immediately
    float angle = 0.0f;
    {
        int scx = START_MX, scy = START_MY;   // 63, 63
        if      (!gMap[scy  ][scx+1]) angle = 0.0f;
        else if (!gMap[scy  ][scx-1]) angle = 3.14159f;
        else if (!gMap[scy+1][scx  ]) angle = 3.14159f / 2.0f;
        else if (!gMap[scy-1][scx  ]) angle = -3.14159f / 2.0f;
    }

    constexpr float MOVE_SPEED = 0.06f;
    constexpr float TURN_SPEED = 0.045f;
    constexpr float MARGIN     = 0.25f;

    rawOn();
    printf("[RAYCASTER] Connected. Use the keys shown above.\n");

    auto lastFrame = std::chrono::high_resolution_clock::now();

    // ── Held-key state ────────────────────────────────────────────────────────
    // Terminal raw mode has no key-up events.  We timestamp every key press
    // and consider it "held" until it hasn't been seen for KEY_HOLD_US µs.
    // OS key-repeat fires ~every 30 ms; 120 ms gives 4× headroom so fast
    // typing doesn't accidentally release a held key mid-stride.
    using clk = std::chrono::steady_clock;
    long long heldAt[256] = {};
    bool      heldOn[256] = {};

    constexpr long long KEY_HOLD_US = 120000LL;

    auto nowUs = [&]() -> long long {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   clk::now().time_since_epoch()).count();
    };
    auto pressKey = [&](unsigned char c) {
        c = (unsigned char)std::tolower(c);
        heldOn[c] = true;
        heldAt[c] = nowUs();
    };
    auto isHeld = [&](unsigned char c) -> bool {
        if (!heldOn[c]) return false;
        if (nowUs() - heldAt[c] > KEY_HOLD_US) { heldOn[c] = false; return false; }
        return true;
    };

    while (running) {
        // ── Input — drain buffer, refresh held timestamps ─────────────────────
        {
            char buf[64]; int n;
            while ((n = (int)read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
                for (int i = 0; i < n; i++) {
                    char c = buf[i];
                    if (c == 'q' || c == 'Q') running = false;
                    else pressKey((unsigned char)c);
                }
            }
        }

        if (isHeld('a')) angle -= TURN_SPEED;
        if (isHeld('d')) angle += TURN_SPEED;

        float newPx = px, newPy = py;
        if (isHeld('w')) { newPx += cosf(angle)*MOVE_SPEED; newPy += sinf(angle)*MOVE_SPEED; }
        if (isHeld('s')) { newPx -= cosf(angle)*MOVE_SPEED; newPy -= sinf(angle)*MOVE_SPEED; }

        // ── Collision — 4-corner AABB, exclusive slide ────────────────────────
        // Try full move → X-only slide → Y-only slide (exclusive, not both).
        // Doing both slides at once lets the player clip around wall corners.
        auto blocked = [&](float cx, float cy) -> bool {
            int x0 = (int)(cx - MARGIN), x1 = (int)(cx + MARGIN);
            int y0 = (int)(cy - MARGIN), y1 = (int)(cy + MARGIN);
            return gMap[y0][x0] || gMap[y0][x1] ||
                   gMap[y1][x0] || gMap[y1][x1];
        };

        if      (!blocked(newPx, newPy)) { px = newPx; py = newPy; }
        else if (!blocked(newPx, py   )) { px = newPx; }
        else if (!blocked(px,    newPy)) { py = newPy; }

        // ── Door interaction (E — rising edge only) ───────────────────────────
        static bool ePrev = false;
        bool eCur = isHeld('e');
        if (eCur && !ePrev) {
            // Step along the facing ray up to 1.5 units, find first door cell
            for (float d = 0.4f; d <= 1.5f; d += 0.3f) {
                int dx = (int)(px + cosf(angle) * d);
                int dy = (int)(py + sinf(angle) * d);
                if ((unsigned)dx >= MAP_W || (unsigned)dy >= MAP_H) break;
                if (!gDoors[dy][dx]) continue;
                if (gMap[dy][dx] == 2) {
                    gMap[dy][dx] = 0;           // open
                } else {
                    // Only close if player centre is >1 unit away (don't trap player)
                    float fdx = (float)dx + 0.5f - px;
                    float fdy = (float)dy + 0.5f - py;
                    if (fdx*fdx + fdy*fdy > 1.0f)
                        gMap[dy][dx] = 2;       // close
                }
                rdr.invalidateRaycasterCache();  // door changed — force full redraw
                break;
            }
        }
        ePrev = eCur;

        // ── Render ────────────────────────────────────────────────────────────
        rdr.beginFrame();
        rdr.raycaster(px, py, angle,
                      0, 0, rdr.physW(), VIEW_H,
                      gMap[0], MAP_W, MAP_H);
        rdr.endFrame();

        // ── ~60 FPS cap ───────────────────────────────────────────────────────
        auto now = std::chrono::high_resolution_clock::now();
        long us  = std::chrono::duration_cast<std::chrono::microseconds>(now - lastFrame).count();
        if (us < 16667) usleep((useconds_t)(16667 - us));
        lastFrame = std::chrono::high_resolution_clock::now();
    }

    rawOff();
    rdr.requestSpiSpeed(0);          // restore driver's configured default speed
    usleep(60000);                   // 60 ms — gives driver one frame cycle to apply it
    rdr.shutdown();
    unlink(PID_FILE);
    printf("\n[RAYCASTER] Exited cleanly.\n");
    return 0;
}
