# Touch Diagnostic Apps - Standalone Makefile
# Direct Hardware Access - No Daemon Required!
#
# Usage:
#   make              - Build all Touch diagnostic apps
#   make calibrate    - Build calibration tool only
#   make draw         - Build drawing app only
#   make debugger     - Build debugger only
#   make clean        - Remove build artifacts
#   make install      - Install to /usr/local/bin (requires sudo)
#   make uninstall    - Remove from /usr/local/bin

CXX = g++

# Detect architecture and set appropriate flags
ARCH := $(shell uname -m)
ifeq ($(ARCH),aarch64)
    # ARM64 (Pi 4/5) - NEON is always available
    CXXFLAGS = -Wall -O3 -march=native -std=c++17 -pthread
else ifeq ($(ARCH),armv7l)
    # ARM32 (Pi 2/3) - need to specify NEON
    CXXFLAGS = -Wall -O3 -march=native -mfpu=neon -std=c++17 -pthread
else
    # Fallback for other architectures
    CXXFLAGS = -Wall -O3 -march=native -std=c++17 -pthread
endif

LDFLAGS = -lpigpio -lrt -pthread

# Directories
DRIVER_DIR = ../../Drivers
CURRENT_DIR = .

# Include paths
INCLUDES = -I$(DRIVER_DIR)

# Source files
CAL_SOURCE = $(CURRENT_DIR)/touch_calibrate_visual_v3.cpp
DRAW_SOURCE = $(CURRENT_DIR)/touch_draw_v3.cpp
DEBUG_SOURCE = $(CURRENT_DIR)/touch_debugger_v3.cpp

# Driver dependencies (shared objects)
DRIVER_OBJS = $(DRIVER_DIR)/ili9486.o $(DRIVER_DIR)/touch_xpt2046_v3.o

# Object files
CAL_OBJ = $(CURRENT_DIR)/touch_calibrate_visual_v3.o
DRAW_OBJ = $(CURRENT_DIR)/touch_draw_v3.o
DEBUG_OBJ = $(CURRENT_DIR)/touch_debugger_v3.o

# Binaries
CAL_BIN = $(CURRENT_DIR)/touch_calibrate_visual_v3
DRAW_BIN = $(CURRENT_DIR)/touch_draw_v3
DEBUG_BIN = $(CURRENT_DIR)/touch_debugger_v3

.PHONY: all calibrate draw debugger clean install uninstall help

# Default target - build all
all: $(CAL_BIN) $(DRAW_BIN) $(DEBUG_BIN)
	@echo ""
	@echo "╔══════════════════════════════════════════════════════╗"
	@echo "║   Touch Diagnostic Apps - Build Complete!           ║"
	@echo "║   Direct Hardware Access - No Daemon Required        ║"
	@echo "╚══════════════════════════════════════════════════════╝"
	@echo ""
	@echo "Built binaries:"
	@echo "  • $(CAL_BIN)"
	@echo "  • $(DRAW_BIN)"
	@echo "  • $(DEBUG_BIN)"
	@echo ""
	@echo "Quick start:"
	@echo "  1. Calibrate:  sudo $(CAL_BIN)"
	@echo "  2. Test:       sudo $(DRAW_BIN)"
	@echo "  3. Debug:      sudo $(DEBUG_BIN)"
	@echo ""
	@echo "Note: All apps require 'sudo' for SPI hardware access"
	@echo ""

# Individual build targets
calibrate: $(CAL_BIN)
	@echo "[TOUCH] Calibration tool ready: $(CAL_BIN)"

draw: $(DRAW_BIN)
	@echo "[TOUCH] Drawing app ready: $(DRAW_BIN)"

debugger: $(DEBUG_BIN)
	@echo "[TOUCH] Debugger ready: $(DEBUG_BIN)"

# Link binaries
$(CAL_BIN): $(CAL_OBJ) $(DRIVER_OBJS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(DRAW_BIN): $(DRAW_OBJ) $(DRIVER_OBJS)
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(DEBUG_BIN): $(DEBUG_OBJ) $(DRIVER_DIR)/touch_xpt2046_v3.o
	@echo "[LINK] $@"
	@$(CXX) $(CXXFLAGS) -o $@ $^ -lrt -lpthread

# Compile Touch app objects
$(CAL_OBJ): $(CAL_SOURCE) $(DRIVER_DIR)/ili9486.h $(DRIVER_DIR)/touch_xpt2046_v3.h $(DRIVER_DIR)/config.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(DRAW_OBJ): $(DRAW_SOURCE) $(DRIVER_DIR)/ili9486.h $(DRIVER_DIR)/touch_xpt2046_v3.h $(DRIVER_DIR)/config.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(DEBUG_OBJ): $(DEBUG_SOURCE) $(DRIVER_DIR)/touch_xpt2046_v3.h $(DRIVER_DIR)/config.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile driver dependencies if needed
$(DRIVER_DIR)/ili9486.o: $(DRIVER_DIR)/ili9486.cpp $(DRIVER_DIR)/ili9486.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(DRIVER_DIR)/touch_xpt2046_v3.o: $(DRIVER_DIR)/touch_xpt2046_v3.cpp $(DRIVER_DIR)/touch_xpt2046_v3.h
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning Touch diagnostic apps..."
	rm -f $(CAL_OBJ) $(DRAW_OBJ) $(DEBUG_OBJ)
	rm -f $(CAL_BIN) $(DRAW_BIN) $(DEBUG_BIN)
	@echo "Clean complete (driver objects preserved)"

# Deep clean - also remove driver objects
clean-all:
	@echo "Deep cleaning (including driver objects)..."
	rm -f $(CAL_OBJ) $(DRAW_OBJ) $(DEBUG_OBJ)
	rm -f $(CAL_BIN) $(DRAW_BIN) $(DEBUG_BIN)
	rm -f $(DRIVER_OBJS)
	@echo "Deep clean complete"

# Install to system
install: $(CAL_BIN) $(DRAW_BIN) $(DEBUG_BIN)
	@echo "Installing Touch diagnostic apps to /usr/local/bin..."
	install -m 755 $(CAL_BIN) /usr/local/bin/
	install -m 755 $(DRAW_BIN) /usr/local/bin/
	install -m 755 $(DEBUG_BIN) /usr/local/bin/
	@echo ""
	@echo "✓ Installation complete!"
	@echo ""
	@echo "You can now run from anywhere:"
	@echo "  sudo touch_calibrate_visual_v3"
	@echo "  sudo touch_draw_v3"
	@echo "  sudo touch_debugger_v3"
	@echo ""

# Uninstall from system
uninstall:
	@echo "Removing Touch diagnostic apps from /usr/local/bin..."
	rm -f /usr/local/bin/touch_calibrate_visual_v3
	rm -f /usr/local/bin/touch_draw_v3
	rm -f /usr/local/bin/touch_debugger_v3
	@echo "Uninstall complete"

# Help
help:
	@echo "Touch Diagnostic Apps - Standalone Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build all Touch diagnostic apps"
	@echo "  make calibrate    - Build calibration tool only"
	@echo "  make draw         - Build drawing app only"
	@echo "  make debugger     - Build debugger only"
	@echo "  make clean        - Remove Touch app build artifacts"
	@echo "  make clean-all    - Also remove driver objects"
	@echo "  make install      - Install to /usr/local/bin (requires sudo)"
	@echo "  make uninstall    - Remove from /usr/local/bin"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "Applications:"
	@echo "  touch_calibrate_visual_v3  - Visual touch calibration (4-point)"
	@echo "  touch_draw_v3              - Drawing app for touch testing"
	@echo "  touch_debugger_v3          - Terminal coordinate debugger"
	@echo ""
	@echo "Requirements:"
	@echo "  • Raspberry Pi with ILI9486 LCD (/dev/spidev0.0)"
	@echo "  • XPT2046 touch controller (/dev/spidev0.1)"
	@echo "  • libpigpio-dev installed"
	@echo "  • SPI enabled (sudo raspi-config)"
	@echo "  • Run with 'sudo' for hardware access"
	@echo ""
	@echo "Quick Start:"
	@echo "  1. Build:      make"
	@echo "  2. Calibrate:  sudo ./touch_calibrate_visual_v3"
	@echo "  3. Test:       sudo ./touch_draw_v3"
	@echo "  4. Debug:      sudo ./touch_debugger_v3"
	@echo ""
