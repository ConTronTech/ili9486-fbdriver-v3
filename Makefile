# ILI9486 V3 Framebuffer Driver Makefile
# 100x Better Performance Edition
# Organized filesystem structure

CXX = g++

# Detect architecture and set appropriate flags
ARCH := $(shell uname -m)
ifeq ($(ARCH),aarch64)
    # ARM64 (Pi 4/5) - NEON is always available, no -mfpu flag needed
    CXXFLAGS = -Wall -O3 -march=native -std=c++17 -pthread
else ifeq ($(ARCH),armv7l)
    # ARM32 (Pi 2/3) - need to specify NEON
    CXXFLAGS = -Wall -O3 -march=native -mfpu=neon -std=c++17 -pthread
else
    # Fallback for other architectures
    CXXFLAGS = -Wall -O3 -march=native -std=c++17 -pthread
endif

LDFLAGS = -lpigpio -lrt -pthread

# Directory structure
DRIVER_DIR = Drivers
APPS_DIR = Apps
TOUCH_DIR = $(APPS_DIR)/Touch
CUBE_DIR = $(APPS_DIR)/Spinning-Cube
IMG_DIR = $(APPS_DIR)/Image-Viewer
VIDEO_DIR = $(APPS_DIR)/Video-Player
TOUCH_TEST_DIR = $(APPS_DIR)/Touch-Test
TOUCH_CAL_DIR = $(APPS_DIR)/Touch-Calibrate
RAYCASTER_DIR = $(APPS_DIR)/Raycaster

# Include paths
INCLUDES = -I$(DRIVER_DIR) -I$(CUBE_DIR) -I$(IMG_DIR)

# Source files
DRIVER_SOURCES = $(DRIVER_DIR)/ili9486.cpp
V3_SOURCES = $(DRIVER_DIR)/fb_ili9486_v3.cpp
MAIN_SOURCE = $(DRIVER_DIR)/fb_main_v3.cpp
AUDIO_SOURCES = $(DRIVER_DIR)/audio_pw_v3.cpp $(DRIVER_DIR)/audio_main_v3.cpp
CUBE_SOURCE = $(CUBE_DIR)/spinning_cube_v3.cpp
IMG_SOURCE = $(IMG_DIR)/image_viewer_v3.cpp
VIDEO_SOURCE = $(VIDEO_DIR)/video_player_v3.cpp
COLORTEST_SOURCE = $(APPS_DIR)/color_test_v3.cpp
TOUCH_SOURCES = $(DRIVER_DIR)/touch_xpt2046_v3.cpp $(DRIVER_DIR)/touch_main_v3.cpp
TOUCH_TEST_SOURCE = $(TOUCH_TEST_DIR)/touch_test_v3.cpp
TOUCH_UI_DEMO_SOURCE = $(TOUCH_TEST_DIR)/touch_ui_demo_v3.cpp
TOUCH_CAL_SOURCE = $(TOUCH_CAL_DIR)/touch_calibrate_v3.cpp
TOUCH_CAL_INT_SOURCE = $(TOUCH_CAL_DIR)/touch_calibrate_interactive_v3.cpp

# New Touch diagnostic apps (Direct Hardware Access)
RAYCASTER_SOURCE = $(RAYCASTER_DIR)/raycaster_v3.cpp

TOUCH_CAL_VISUAL_SOURCE = $(TOUCH_DIR)/touch_calibrate_visual_v3.cpp
TOUCH_DRAW_SOURCE = $(TOUCH_DIR)/touch_draw_v3.cpp
TOUCH_DEBUGGER_SOURCE = $(TOUCH_DIR)/touch_debugger_v3.cpp

# Object files (build in same directory as source)
DRIVER_OBJS = $(DRIVER_DIR)/ili9486.o
V3_OBJS = $(DRIVER_DIR)/fb_ili9486_v3.o
MAIN_OBJ = $(DRIVER_DIR)/fb_main_v3.o
AUDIO_OBJS = $(DRIVER_DIR)/audio_pw_v3.o $(DRIVER_DIR)/audio_main_v3.o
CUBE_OBJ = $(CUBE_DIR)/spinning_cube_v3.o
IMG_OBJ = $(IMG_DIR)/image_viewer_v3.o
VIDEO_OBJ = $(VIDEO_DIR)/video_player_v3.o
COLORTEST_OBJ = $(APPS_DIR)/color_test_v3.o
TOUCH_OBJS = $(DRIVER_DIR)/touch_xpt2046_v3.o $(DRIVER_DIR)/touch_main_v3.o
TOUCH_TEST_OBJ = $(TOUCH_TEST_DIR)/touch_test_v3.o
TOUCH_UI_DEMO_OBJ = $(TOUCH_TEST_DIR)/touch_ui_demo_v3.o
TOUCH_CAL_OBJ = $(TOUCH_CAL_DIR)/touch_calibrate_v3.o
TOUCH_CAL_DRIVER_OBJ = $(DRIVER_DIR)/touch_xpt2046_v3_cal.o
TOUCH_CAL_INT_OBJ = $(TOUCH_CAL_DIR)/touch_calibrate_interactive_v3.o
TOUCH_CAL_INT_DRIVER_OBJ = $(DRIVER_DIR)/touch_xpt2046_v3_cal_int.o

# New Touch app objects
RENDER_OBJ    = $(DRIVER_DIR)/render_v3.o
RAYCASTER_OBJ = $(RAYCASTER_DIR)/raycaster_v3.o

TOUCH_CAL_VISUAL_OBJ = $(TOUCH_DIR)/touch_calibrate_visual_v3.o
TOUCH_DRAW_OBJ = $(TOUCH_DIR)/touch_draw_v3.o
TOUCH_DEBUGGER_OBJ = $(TOUCH_DIR)/touch_debugger_v3.o

# Targets (binaries in their respective folders)
DRIVER_BIN = $(DRIVER_DIR)/fb_main_v3
AUDIO_BIN  = $(DRIVER_DIR)/audio_main_v3
CUBE_BIN = $(CUBE_DIR)/spinning_cube_v3
IMG_BIN = $(IMG_DIR)/image_viewer_v3
VIDEO_BIN = $(VIDEO_DIR)/video_player_v3
COLORTEST_BIN = $(APPS_DIR)/color_test_v3
TOUCH_BIN = $(DRIVER_DIR)/touch_main_v3
TOUCH_TEST_BIN = $(TOUCH_TEST_DIR)/touch_test_v3
TOUCH_UI_DEMO_BIN = $(TOUCH_TEST_DIR)/touch_ui_demo_v3
TOUCH_CAL_BIN = $(TOUCH_CAL_DIR)/touch_calibrate_v3
TOUCH_CAL_INT_BIN = $(TOUCH_CAL_DIR)/touch_calibrate_interactive_v3

# New Touch diagnostic binaries
RAYCASTER_BIN = $(RAYCASTER_DIR)/raycaster_v3

TOUCH_CAL_VISUAL_BIN = $(TOUCH_DIR)/touch_calibrate_visual_v3
TOUCH_DRAW_BIN = $(TOUCH_DIR)/touch_draw_v3
TOUCH_DEBUGGER_BIN = $(TOUCH_DIR)/touch_debugger_v3

.PHONY: all clean install uninstall test help

all: $(DRIVER_BIN) $(AUDIO_BIN) $(CUBE_BIN) $(IMG_BIN) $(VIDEO_BIN) $(TOUCH_BIN) $(TOUCH_CAL_VISUAL_BIN) $(TOUCH_DRAW_BIN) $(TOUCH_DEBUGGER_BIN) $(RAYCASTER_BIN)
	@echo ""
	@echo "╔═══════════════════════════════════════════════╗"
	@echo "║   Build Complete - V3 Edition                 ║"
	@echo "║   Organized Filesystem Structure              ║"
	@echo "╚═══════════════════════════════════════════════╝"
	@echo ""
	@echo "Binaries (organized by folder):"
	@echo "  • $(DRIVER_BIN)  - Main driver"
	@echo "  • $(CUBE_BIN)  - Spinning cube"
	@echo "  • $(IMG_BIN)  - Image viewer"
	@echo "  • $(VIDEO_BIN)  - Video player (FFmpeg)"
	@echo "  • $(TOUCH_BIN)  - Touch controller driver"
	@echo ""
	@echo "NEW Touch Diagnostic Apps (Direct Hardware - No Daemon!):"
	@echo "  • $(TOUCH_CAL_VISUAL_BIN)  - Visual calibration (RECOMMENDED)"
	@echo "  • $(TOUCH_DRAW_BIN)  - Drawing app for touch testing"
	@echo "  • $(TOUCH_DEBUGGER_BIN)  - Terminal coordinate debugger"
	@echo ""
	@echo "Quick start:"
	@echo "  1. sudo $(DRIVER_BIN)  (terminal 1)"
	@echo "  2. $(CUBE_BIN)  (terminal 2)"
	@echo "  3. $(IMG_BIN) Apps/Image-Viewer/pics/Siamese-Cat.png fill"
	@echo "  4. $(VIDEO_BIN) video.mp4  (requires FFmpeg libraries)"
	@echo "  5. $(COLORTEST_BIN)  (color accuracy test)"
	@echo ""
	@echo "Touch controller:"
	@echo "  1. sudo $(TOUCH_BIN)  (terminal 3 - if you have XPT2046 touch screen)"
	@echo "  2. sudo $(TOUCH_DRAW_BIN)  (direct hardware drawing app)"
	@echo "  3. sudo $(TOUCH_DEBUGGER_BIN)  (terminal coordinate debugger)"
	@echo ""
	@echo "Install system-wide:"
	@echo "  sudo make install"
	@echo ""

# Build driver binary
$(DRIVER_BIN): $(DRIVER_OBJS) $(V3_OBJS) $(MAIN_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Build audio driver
PIPEWIRE_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3)
PIPEWIRE_LIBS   := $(shell pkg-config --libs   libpipewire-0.3)

$(AUDIO_BIN): $(AUDIO_OBJS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lpthread $(PIPEWIRE_LIBS)

$(DRIVER_DIR)/audio_pw_v3.o: $(DRIVER_DIR)/audio_pw_v3.cpp $(DRIVER_DIR)/audio_pw_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(PIPEWIRE_CFLAGS) $(INCLUDES) -c $< -o $@

$(DRIVER_DIR)/audio_main_v3.o: $(DRIVER_DIR)/audio_main_v3.cpp $(DRIVER_DIR)/audio_pw_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Build example cube
$(CUBE_BIN): $(CUBE_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt

# Build image viewer
$(IMG_BIN): $(IMG_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt

# Build color test
$(COLORTEST_BIN): $(COLORTEST_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt

# Build video player
$(VIDEO_BIN): $(VIDEO_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lavformat -lavcodec -lavutil -lswscale -lswresample

# Build touch driver
$(TOUCH_BIN): $(TOUCH_OBJS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lpthread

# Build touch test app
$(TOUCH_TEST_BIN): $(TOUCH_TEST_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lpthread

# Build touch UI demo
$(TOUCH_UI_DEMO_BIN): $(TOUCH_UI_DEMO_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lpthread

# Build touch calibration tool
$(TOUCH_CAL_BIN): $(TOUCH_CAL_OBJ) $(TOUCH_CAL_DRIVER_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lpthread

# Build interactive calibration tool
$(TOUCH_CAL_INT_BIN): $(TOUCH_CAL_INT_OBJ) $(TOUCH_CAL_INT_DRIVER_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lpthread

# Build NEW Touch diagnostic apps (Direct Hardware Access)
$(TOUCH_CAL_VISUAL_BIN): $(TOUCH_CAL_VISUAL_OBJ) $(DRIVER_OBJS) $(DRIVER_DIR)/touch_xpt2046_v3.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TOUCH_DRAW_BIN): $(TOUCH_DRAW_OBJ) $(DRIVER_OBJS) $(DRIVER_DIR)/touch_xpt2046_v3.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TOUCH_DEBUGGER_BIN): $(TOUCH_DEBUGGER_OBJ) $(DRIVER_DIR)/touch_xpt2046_v3.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lpthread

# Build render engine object (shared by all apps that use RenderV3)
$(RENDER_OBJ): $(DRIVER_DIR)/render_v3.cpp $(DRIVER_DIR)/render_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Build raycaster (links against render engine)
$(RAYCASTER_BIN): $(RAYCASTER_OBJ) $(RENDER_OBJ)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lm

$(RAYCASTER_DIR)/raycaster_v3.o: $(RAYCASTER_DIR)/raycaster_v3.cpp $(DRIVER_DIR)/render_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -I$(DRIVER_DIR) -c $< -o $@

# Compile driver objects
$(DRIVER_DIR)/ili9486.o: $(DRIVER_DIR)/ili9486.cpp $(DRIVER_DIR)/ili9486.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(DRIVER_DIR)/fb_ili9486_v3.o: $(DRIVER_DIR)/fb_ili9486_v3.cpp $(DRIVER_DIR)/fb_ili9486_v3.h $(DRIVER_DIR)/ili9486.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(DRIVER_DIR)/fb_main_v3.o: $(DRIVER_DIR)/fb_main_v3.cpp $(DRIVER_DIR)/fb_ili9486_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile app objects
$(CUBE_DIR)/spinning_cube_v3.o: $(CUBE_DIR)/spinning_cube_v3.cpp
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(IMG_DIR)/image_viewer_v3.o: $(IMG_DIR)/image_viewer_v3.cpp
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(APPS_DIR)/color_test_v3.o: $(APPS_DIR)/color_test_v3.cpp
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(VIDEO_DIR)/video_player_v3.o: $(VIDEO_DIR)/video_player_v3.cpp
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile touch driver objects
$(DRIVER_DIR)/touch_xpt2046_v3.o: $(DRIVER_DIR)/touch_xpt2046_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(DRIVER_DIR)/touch_main_v3.o: $(DRIVER_DIR)/touch_main_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(TOUCH_TEST_DIR)/touch_test_v3.o: $(TOUCH_TEST_DIR)/touch_test_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(TOUCH_TEST_DIR)/touch_ui_demo_v3.o: $(TOUCH_TEST_DIR)/touch_ui_demo_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(TOUCH_CAL_DIR)/touch_calibrate_v3.o: $(TOUCH_CAL_DIR)/touch_calibrate_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(DRIVER_DIR)/touch_xpt2046_v3_cal.o: $(DRIVER_DIR)/touch_xpt2046_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h
	@echo "[CXX] $< (for calibration)"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(TOUCH_CAL_DIR)/touch_calibrate_interactive_v3.o: $(TOUCH_CAL_DIR)/touch_calibrate_interactive_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(DRIVER_DIR)/touch_xpt2046_v3_cal_int.o: $(DRIVER_DIR)/touch_xpt2046_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h
	@echo "[CXX] $< (for interactive cal)"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile NEW Touch diagnostic app objects
$(TOUCH_DIR)/touch_calibrate_visual_v3.o: $(TOUCH_DIR)/touch_calibrate_visual_v3.cpp $(DRIVER_DIR)/ili9486.h $(DRIVER_DIR)/touch_xpt2046_v3.h $(DRIVER_DIR)/config.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -I$(DRIVER_DIR) -c $< -o $@

$(TOUCH_DIR)/touch_draw_v3.o: $(TOUCH_DIR)/touch_draw_v3.cpp $(DRIVER_DIR)/ili9486.h $(DRIVER_DIR)/touch_xpt2046_v3.h $(DRIVER_DIR)/config.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -I$(DRIVER_DIR) -c $< -o $@

$(TOUCH_DIR)/touch_debugger_v3.o: $(TOUCH_DIR)/touch_debugger_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h $(DRIVER_DIR)/config.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -I$(DRIVER_DIR) -c $< -o $@

# Installation
install: $(DRIVER_BIN) $(AUDIO_BIN) $(CUBE_BIN) $(IMG_BIN) $(TOUCH_BIN) $(TOUCH_TEST_BIN)
	@echo "Installing V3 driver system-wide..."
	install -m 755 $(DRIVER_BIN) /usr/local/bin/
	install -m 755 $(AUDIO_BIN) /usr/local/bin/
	install -m 755 $(CUBE_BIN) /usr/local/bin/
	install -m 755 $(IMG_BIN) /usr/local/bin/
	install -m 755 $(TOUCH_BIN) /usr/local/bin/
	install -m 755 $(TOUCH_TEST_BIN) /usr/local/bin/
	@echo ""
	@echo "Installation complete!"
	@echo "Run 'sudo fb_main_v3' to start the display driver"
	@echo "Run 'audio_main_v3' to start the audio driver (no sudo needed)"
	@echo "Run 'sudo touch_main_v3' to start the touch driver (if you have XPT2046)"

# Uninstall
uninstall:
	@echo "Removing V3 driver..."
	rm -f /usr/local/bin/fb_main_v3
	rm -f /usr/local/bin/audio_main_v3
	rm -f /usr/local/bin/spinning_cube_v3
	rm -f /usr/local/bin/image_viewer_v3
	rm -f /usr/local/bin/touch_main_v3
	rm -f /usr/local/bin/touch_test_v3
	@echo "Uninstall complete"

# Test (runs driver briefly then stops)
test: $(DRIVER_BIN)
	@echo "Testing driver initialization..."
	sudo timeout 5 ./$(DRIVER_BIN) || true
	@echo "Test complete"

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(DRIVER_OBJS) $(V3_OBJS) $(MAIN_OBJ) $(AUDIO_OBJS) $(CUBE_OBJ) $(IMG_OBJ) $(VIDEO_OBJ) $(TOUCH_OBJS) $(TOUCH_CAL_VISUAL_OBJ) $(TOUCH_DRAW_OBJ) $(TOUCH_DEBUGGER_OBJ) $(RAYCASTER_OBJ) $(RENDER_OBJ)
	rm -f $(DRIVER_BIN) $(AUDIO_BIN) $(CUBE_BIN) $(IMG_BIN) $(VIDEO_BIN) $(TOUCH_BIN) $(TOUCH_CAL_VISUAL_BIN) $(TOUCH_DRAW_BIN) $(TOUCH_DEBUGGER_BIN) $(RAYCASTER_BIN)
	@echo "Clean complete"

# Help
help:
	@echo "ILI9486 V3 Framebuffer Driver - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make           - Build everything"
	@echo "  make install   - Install system-wide (requires sudo)"
	@echo "  make uninstall - Remove from system (requires sudo)"
	@echo "  make test      - Test driver initialization"
	@echo "  make clean     - Remove build artifacts"
	@echo "  make help      - Show this help"
	@echo ""
	@echo "Directory Structure:"
	@echo "  Drivers/            - Core driver files"
	@echo "  Apps/Spinning-Cube/ - Spinning cube demo"
	@echo "  Apps/Image-Viewer/  - Image viewer app"
	@echo "  Backups/            - Backup files"
	@echo ""
	@echo "Compiler flags:"
	@echo "  -O3            - Maximum optimization"
	@echo "  -march=native  - CPU-specific optimizations"
	@echo "  -std=c++17     - C++17 features"
	@echo ""
