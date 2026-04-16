# ILI9486 Framebuffer Driver V3

**100x Better Performance** - Zero-copy shared memory, NEON SIMD, dual-threaded pipeline, automatic dirty detection

## What's New in V3?

This is a **complete rewrite** of the ILI9486 driver with aggressive optimizations:

### Performance Features
- **Zero-Copy Shared Memory** - Apps write directly to shared memory, no file I/O
- **NEON SIMD** - ARM NEON instructions for fast dirty-region detection
- **Dual-Threaded Pipeline** - Separate threads for dirty detection and SPI transfer
- **Two-Pass Dirty Detection** - Fast pixel-level scan finds exact changed region
- **Automatic Dirty Detection** - Only updates changed screen regions via partial SPI writes
- **Adaptive Frame Skipping** - Automatically skips frames when overloaded
- **Thermal Management** - Monitors CPU temp and throttles FPS/SPI speed to protect hardware
- **Real-time Stats** - FPS, frame count, dirty pixels, update time

### Developer Features
- **Double-Buffered Shared Memory** - Two framebuffers (`_0` and `_1`) with atomic index switching
- **Single-Instance Guard** - All programs refuse to start if already running, prints the PID to kill
- **Drop-in Integration** - Just mmap shared memory and write RGB565 pixels

## Quick Start

### Prerequisites
```bash
sudo apt-get install libpigpio-dev pigpio
sudo raspi-config   # Interface Options → SPI → Enable
```

### Build
```bash
cd Main_v3_newist
make
```

### Run
```bash
# Terminal 1: Start the driver (must be root for SPI/GPIO)
sudo ./Drivers/fb_main_v3

# Terminal 2: Run an app
./Apps/Spinning-Cube/spinning_cube_v3
./Apps/Image-Viewer/image_viewer_v3 Apps/Image-Viewer/pics/Siamese-Cat.png fill
./Apps/Video-Player/video_player_v3 video.mp4
```

### Daemon Mode
```bash
sudo ./Drivers/fb_main_v3 --daemon   # runs in background, prints PID
sudo kill <pid>                       # stop it
```

## Command Line Options

```
sudo ./Drivers/fb_main_v3 [OPTIONS]

  --daemon, -d     Run as background daemon
  --no-dirty       Disable automatic dirty detection (full-screen updates)
  --no-skip        Disable frame skipping
  --verbose, -v    Enable verbose logging
  --help, -h       Show help
```

## Writing an App

### 1. Map shared memory
```cpp
#include <sys/mman.h>
#include <fcntl.h>

#define LCD_WIDTH  320
#define LCD_HEIGHT 480
#define FB_SHM     "/ili9486_fb_v3_0"
#define STATS_SHM  "/ili9486_fb_v3_stats"

// Map framebuffer (buffer 0)
int fd = shm_open(FB_SHM, O_RDWR, 0666);
uint16_t* fb = (uint16_t*)mmap(nullptr, LCD_WIDTH * LCD_HEIGHT * 2,
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
close(fd);

// Write pixels in RGB565 — driver converts to BGR565 for the display
fb[y * LCD_WIDTH + x] = 0xF800;  // red pixel
```

### 2. Use frameInProgress for atomic frames
```cpp
// Signal draw in progress — driver pauses dirty detection during erase+draw
stats->frameInProgress.store(true, std::memory_order_seq_cst);
std::atomic_thread_fence(std::memory_order_seq_cst);

clearOldContent(fb);
drawNewContent(fb);

std::atomic_thread_fence(std::memory_order_seq_cst);
stats->frameInProgress.store(false, std::memory_order_seq_cst);
```

### 3. Lock to a single buffer
```cpp
// Dirty detection compares each buffer against its own previousFrame snapshot.
// Switching buffers mid-stream breaks this model — always lock to one buffer.
stats->activeBufferIndex.store(0, std::memory_order_seq_cst);
```

## Color Format

Apps write **RGB565** — the driver converts to BGR565 for the display:
```cpp
#define COLOR_RED    0xF800
#define COLOR_GREEN  0x07E0
#define COLOR_BLUE   0x001F
#define COLOR_WHITE  0xFFFF
#define COLOR_BLACK  0x0000
#define COLOR_YELLOW 0xFFE0
#define COLOR_CYAN   0x07FF
#define COLOR_MAGENTA 0xF81F
```

## Configuration — `Drivers/settings.conf`

Key settings:

| Key | Default | Notes |
|-----|---------|-------|
| `spi_speed` | `32000000` | 32 MHz rated; 64 MHz tested overclock |
| `dirty_detection` | `1` | 0 = full-screen every frame |
| `dirty_threshold` | `0.90` | Switch to full update if >90% dirty |
| `orientation` | `0` | 0=landscape, 90=portrait, 180, 270 |
| `gpio_dc` | `24` | BCM pin for Data/Command |
| `gpio_rst` | `25` | BCM pin for Reset |
| `target_fps` | `60` | Logical cap (SPI speed is real limit) |

## Performance

**At 32 MHz SPI:**
- Small dirty regions (spinning cube): 28–30 FPS
- Full screen updates (video): ~9 FPS — hardware limit (~111 ms per frame)
- Dirty detection overhead: ~1–2 ms per frame (NEON optimised)

**At 64 MHz SPI (overclock):**
- Full screen: ~18 FPS
- Small dirty regions: 50+ FPS
- NOT RECOMENDED

## Architecture

```
App Thread                   Driver Thread 1              Driver Thread 2
──────────                   (Dirty Detection)            (SPI Transfer)

stats->frameInProgress = true

Erase old content
Draw new content

stats->frameInProgress = false
                             Wait for frameInProgress=false

                             Snapshot active framebuffer

                             Two-pass dirty scan:
                               Pass 1 — every 2nd pixel, rough bounds
                               Pass 2 — every pixel in region, exact bounds

                             Pack dirty rows → transferBuf
                                                          Receive frameReady

                                                          lcd.updateRegionFromBuffer()
                                                            (partial SPI write)

                                                          Update FPS stats
```

## Shared Memory Names

| Name | Contents |
|------|----------|
| `/ili9486_fb_v3_0` | Framebuffer 0 (320×480 × 2 bytes) |
| `/ili9486_fb_v3_1` | Framebuffer 1 (320×480 × 2 bytes) |
| `/ili9486_fb_v3_stats` | `FramebufferStats` struct (atomics) |
| `/xpt2046_touch_v3` | Touch event queue (64 events) |
| `/xpt2046_touch_v3_stats` | Touch stats struct |

## Troubleshooting

**"Failed to open shared memory"** — `fb_main_v3` isn't running. Start it first.

**"Already running (PID 1234)"** — another instance is live. `sudo kill 1234` then retry.

**Colors wrong** — apps must write RGB565. Driver converts to BGR565 internally.

**Low FPS on video** — at 32 MHz SPI, video is hardware-limited to ~9 FPS. Use 64 MHz SPI if your display tolerates it (`spi_speed = 64000000` in settings.conf).

**Display artifacts after app switch** — the driver automatically clears the display when `activeBufferIndex` changes, so ghost content from the previous app is cleared on the first frame.

**Ghost content / two cubes visible** — two instances of an app running simultaneously. Each program prints a "Already running (PID X)" guard — check for stale processes with `ps aux | grep <name>`.

## Touch Controller (XPT2046)

```bash
sudo ./Drivers/touch_main_v3          # start touch daemon
sudo ./Apps/Touch/touch_draw_v3       # drawing app
sudo ./Apps/Touch/touch_calibrate_visual_v3   # 9-point calibration
```

See CLAUDE.md for full touch documentation.

## License

Same as parent project. Use freely for your old 3.5" displays!
