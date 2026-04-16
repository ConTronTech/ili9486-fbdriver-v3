# Touch Diagnostic Apps - Direct Hardware Access

Three standalone touch diagnostic applications that access ILI9486 display and XPT2046 touch controller directly - **NO DAEMON REQUIRED!**

## ⚠️ IMPORTANT: Daemon Conflict

These apps use **Direct Hardware Access** and **cannot run while daemons are active**.

**Before running these apps:**
```bash
# Stop all daemons (if running)
sudo killall fb_main_v3 touch_main_v3 pigpiod

# Then run the app
sudo ./touch_draw_v3
```

**Error "Can't lock /var/run/pigpio.pid"?** → See troubleshooting section below

---

## Applications

### 1. Visual Touch Calibration (`touch_calibrate_visual_v3`)
**Purpose:** Calibrate XPT2046 touch controller with visual on-screen feedback

**Features:**
- Displays crosshairs at 4 corners + center
- Collects 10 averaged samples per calibration point
- Shows raw ADC values and transformed coordinates in real-time
- Automatically saves calibration to `../../Drivers/settings.conf`
- Visual feedback (red = waiting, green = sampling, green flash = success)

**Usage:**
```bash
sudo ./touch_calibrate_visual_v3
```

**Process:**
1. Blue flash confirms display is working
2. Touch each red crosshair as it appears
3. Hold touch steady while it turns green (collecting samples)
4. Calibration values automatically saved to `settings.conf`
5. Test with `touch_draw_v3`

**Output Example:**
```
[CALIBRATE] Calculated Calibration Values:
[CALIBRATE] touch_cal_x_min = 292
[CALIBRATE] touch_cal_x_max = 3868
[CALIBRATE] touch_cal_y_min = 258
[CALIBRATE] touch_cal_y_max = 3875
```

---

### 2. Drawing App (`touch_draw_v3`)
**Purpose:** Test touch accuracy and responsiveness with free-form drawing

**Features:**
- Draw continuous lines by dragging finger
- Color palette (tap top edge to change colors: White, Red, Green, Blue, Yellow, Cyan, Magenta, Orange)
- Clear button (tap center button at top)
- Real-time FPS display in terminal
- Demonstrates proper TOUCH_HELD throttling (~30 FPS)

**Usage:**
```bash
sudo ./touch_draw_v3
```

**Controls:**
- **Drag finger** - Draw lines
- **Tap top edge** - Select color from palette
- **Tap center button** - Clear canvas
- **Ctrl+C** - Exit

**Perfect for:**
- Verifying calibration accuracy
- Testing touch responsiveness
- Checking for dead zones
- Demonstrating touch latency

---

### 3. Touch Debugger (`touch_debugger_v3`)
**Purpose:** Real-time terminal output of touch coordinates for debugging

**Features:**
- Terminal-only (no display driver needed)
- Shows both transformed (display) coordinates AND raw ADC values
- Touch state tracking (PRESSED/HELD/RELEASED)
- Pressure/Z value monitoring
- Touch event counter

**Usage:**
```bash
sudo ./touch_debugger_v3
```

**Output Example:**
```
[PRESSED ] Display(240, 160) Pressure= 842 | Raw ADC(2150, 1890, 842)  [Touch #1]
[HELD    ] Display(242, 162) Pressure= 798 | Raw ADC(2168, 1912, 798)
[HELD    ] Display(245, 165) Pressure= 756 | Raw ADC(2192, 1945, 756)
[RELEASED] Display(245, 165) Pressure=   0 | Raw ADC(   0,    0,   0)
```

**Perfect for:**
- Debugging calibration issues
- Verifying coordinate transforms
- Checking pressure sensitivity
- Confirming orientation settings

---

## Building

### Build All Apps
```bash
cd Apps/Touch
make
```

### Build Individual Apps
```bash
make calibrate    # Build calibration tool only
make draw         # Build drawing app only
make debugger     # Build debugger only
```

### Clean Build Artifacts
```bash
make clean        # Remove Touch app objects/binaries
make clean-all    # Also remove driver objects
```

### Install System-Wide
```bash
sudo make install
```
Installs to `/usr/local/bin/` so you can run from anywhere:
```bash
sudo touch_calibrate_visual_v3
sudo touch_draw_v3
sudo touch_debugger_v3
```

---

## Requirements

### Hardware
- Raspberry Pi 2/3/4/5
- ILI9486 3.5" LCD on `/dev/spidev0.0` (GPIO24=DC, GPIO25=RST)
- XPT2046 touch controller on `/dev/spidev0.1`

### Software
```bash
# Install pigpio library
sudo apt-get update
sudo apt-get install libpigpio-dev pigpio

# Enable SPI
sudo raspi-config
# Interface Options → SPI → Enable
```

### Permissions
All apps require **sudo** for SPI hardware access:
```bash
sudo ./touch_calibrate_visual_v3   # ✓ Correct
./touch_calibrate_visual_v3        # ✗ Won't work - need sudo
```

---

## Architecture - Direct Hardware Access

Unlike daemon-based apps that rely on `fb_main_v3` and `touch_main_v3` running in background, these apps control hardware directly:

```
Traditional Approach (Daemon):
┌─────────┐     ┌──────────────┐     ┌──────────┐
│   App   │ ──> │ Shared Memory│ <── │ Daemon   │
└─────────┘     │ /dev/shm/*   │     │(fb_main) │
                └──────────────┘     └─────┬────┘
                                           │
                                     ┌─────▼────┐
                                     │ Hardware │
                                     └──────────┘

Direct Hardware Approach (These Apps):
┌─────────┐
│   App   │
│ (sudo)  │
└────┬────┘
     │
┌────▼────┐
│ Hardware│
└─────────┘
```

**Advantages:**
- ✓ No daemon dependency
- ✓ Simpler deployment
- ✓ Easier debugging
- ✓ Lower latency
- ✓ Single app ownership

**Trade-offs:**
- Requires sudo (direct SPI access)
- One app at a time (exclusive hardware control)
- Can't use with daemon-based apps simultaneously

---

## Configuration

Apps read from `../../Drivers/settings.conf`:

```conf
# Display settings
orientation = 0              # 0/90/180/270 degrees
spi_speed = 32000000         # 32MHz (safe)

# Touch controller
touch_spi_device = /dev/spidev0.1
touch_spi_speed = 2000000    # 2MHz for XPT2046

# Calibration (set by touch_calibrate_visual_v3)
touch_cal_x_min = 292
touch_cal_x_max = 3868
touch_cal_y_min = 258
touch_cal_y_max = 3875

# Touch sensitivity
touch_pressure_threshold = 1200   # Lower Z1 = valid touch
touch_poll_rate = 100             # 100Hz polling

# Touch dimensions
touch_native_width = 320          # XPT2046 native (portrait)
touch_native_height = 480
touch_display_width = 480         # After orientation transform
touch_display_height = 320
```

---

## Troubleshooting

### "Failed to initialize LCD/touch"
**Cause:** Not running with sudo or SPI not enabled

**Fix:**
```bash
# 1. Use sudo
sudo ./touch_calibrate_visual_v3

# 2. Enable SPI
sudo raspi-config
# Interface Options → SPI → Enable
sudo reboot
```

---

### "Can't lock /var/run/pigpio.pid" or "pigpio uninitialised"
**Cause:** pigpio daemon (pigpiod) or other display/touch daemons are running

This is the most common issue! Direct Hardware apps cannot run while daemons are active because they both try to control the same GPIO hardware.

**Fix:**
```bash
# Stop all daemons
sudo killall fb_main_v3 touch_main_v3 pigpiod

# Verify they're stopped
ps aux | grep -E '(fb_main|touch_main|pigpiod)'

# Now run your Direct Hardware app
sudo ./touch_draw_v3
```

**When to use which approach:**
- **Direct Hardware apps** (`touch_draw_v3`, etc.) - Run standalone, no daemons needed
  - Pros: Simpler, lower latency, easier debugging
  - Cons: One app at a time, requires stopping daemons

- **Daemon-based apps** (`fb_main_v3` + apps) - Run with background daemons
  - Pros: Multiple apps can share display/touch
  - Cons: More complex setup, slightly higher latency

**Choose one approach at a time** - you cannot mix Direct Hardware and daemon-based apps!

---

### "Permission denied on /dev/spidev0.0"
**Cause:** Need root access for SPI

**Fix:** Always use `sudo`

---

### Touch coordinates don't match display
**Cause:** Orientation mismatch or calibration issue

**Fix:**
1. Check `orientation` in `settings.conf` matches physical display rotation
2. Verify `touch_display_width` and `touch_display_height` match current orientation
3. Re-run calibration: `sudo ./touch_calibrate_visual_v3`

---

### Touch not responding
**Cause:** Pressure threshold too strict or wiring issue

**Fix:**
1. Check wiring (TP_CS to GPIO 8, MISO/MOSI/CLK shared with LCD)
2. Increase `touch_pressure_threshold` in `settings.conf` (try 2000-3000)
3. Use debugger to see raw values: `sudo ./touch_debugger_v3`

---

### Drawing app shows wrong colors
**Cause:** Display orientation mismatch

**Fix:**
1. Verify `orientation` in `settings.conf`
2. Set correct `spi_speed` (32MHz safe, 64MHz+ can cause color issues)

---

### Compilation errors
**Common issues:**

**"ili9486.h: No such file"**
```bash
# Check you're in Apps/Touch/ directory
cd Apps/Touch
make
```

**"undefined reference to `gpioInitialise`"**
```bash
# Install pigpio
sudo apt-get install libpigpio-dev
```

---

## Performance Characteristics

### Calibration Tool
- **Calibration time:** ~30-60 seconds (5 points × 10 samples)
- **Accuracy:** ±2-3 pixels with 10-sample averaging
- **Display update:** Full-screen refresh per calibration point

### Drawing App
- **Touch latency:** <20ms from physical touch to visual feedback
- **Drawing FPS:** ~25-30 FPS during continuous drag (throttled)
- **Idle FPS:** 100Hz touch polling (minimal CPU)
- **Line quality:** Smooth with Bresenham's algorithm

### Debugger
- **Polling rate:** 100Hz (configurable in code)
- **Console update:** Real-time for PRESS/HELD, suppressed when RELEASED
- **CPU usage:** Minimal (terminal output only)

---

## Implementation Details

### Color Format
- **Apps write:** RGB565 (standard format)
- **Driver converts:** BGR565 (ILI9486 native)
- Conversion handled in `ili9486.cpp` using NEON SIMD on ARM64

### Touch Coordinate Pipeline
```
Raw ADC (0-4095)
    ↓
calibratePosition() - Maps to native sensor coords (320x480)
    ↓
transformForDisplay() - Rotates based on orientation
    ↓
Display Coordinates (ready to use)
```

### Frame Throttling (Drawing App)
```cpp
#define DRAW_THROTTLE_MS 33  // ~30 FPS

if (event.state == TOUCH_HELD) {
    // Draw to framebuffer every time
    drawLine(fb, lastX, lastY, currentX, currentY);

    // But only update LCD every 33ms
    if (elapsed >= DRAW_THROTTLE_MS) {
        lcd.updateFullScreen(fb);
    }
}
```

This prevents SPI saturation during fast drags while maintaining smooth drawing.

---

## Comparison with Legacy Apps

| Feature | Legacy (Daemon-based) | New (Direct Hardware) |
|---------|----------------------|----------------------|
| **Dependency** | Requires fb_main_v3 + touch_main_v3 | Standalone |
| **Permissions** | App: none, Daemon: sudo | App: sudo |
| **Latency** | Higher (shared memory IPC) | Lower (direct) |
| **Complexity** | Complex (3 processes) | Simple (1 process) |
| **Multi-app** | ✓ Multiple apps can share | ✗ Exclusive access |
| **Debugging** | Harder (3 processes) | Easier (1 process) |
| **Calibration** | Manual config edit | Auto-save to config |

**Recommendation:** Use these Direct Hardware apps for calibration and testing. Use daemon-based apps for production deployments with multiple applications.

---

## Source Code Structure

```
Apps/Touch/
├── Makefile                           # Standalone build system
├── README.md                          # This file
├── touch_calibrate_visual_v3.cpp      # 420 lines - Visual calibration
├── touch_draw_v3.cpp                  # 380 lines - Drawing app
└── touch_debugger_v3.cpp              # 150 lines - Terminal debugger

Dependencies:
├── ../../Drivers/ili9486.cpp          # ILI9486 LCD driver
├── ../../Drivers/ili9486.h
├── ../../Drivers/touch_xpt2046_v3.cpp # XPT2046 touch driver
├── ../../Drivers/touch_xpt2046_v3.h
├── ../../Drivers/config.h             # Config file parser
└── ../../Drivers/settings.conf        # Runtime configuration
```

---

## Quick Reference

### Typical Workflow
```bash
# 1. Build
cd Apps/Touch
make

# 2. Calibrate (REQUIRED FIRST TIME)
sudo ./touch_calibrate_visual_v3

# 3. Test accuracy
sudo ./touch_draw_v3

# 4. Debug if issues
sudo ./touch_debugger_v3

# 5. Optional: Install system-wide
sudo make install
```

### One-Liner Setup
```bash
cd Apps/Touch && make && sudo ./touch_calibrate_visual_v3
```

---

## Support

**For bugs or issues:**
1. Run debugger to see raw coordinates: `sudo ./touch_debugger_v3`
2. Check `../../Drivers/settings.conf` for configuration
3. Verify SPI is enabled: `ls /dev/spidev*`
4. Check pigpio is installed: `dpkg -l | grep pigpio`

**For calibration problems:**
1. Use debugger to verify raw ADC values are sensible (200-3900 range)
2. Check pressure threshold isn't too strict
3. Ensure display orientation matches physical rotation

---

Built with ❤️ for ILI9486 + XPT2046 on Raspberry Pi
