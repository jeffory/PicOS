# PicOS Makefile
# Automated setup, build, and deployment for ClockworkPi PicoCalc

.PHONY: help setup build clean flash rebuild check-env test-lua simulator simulator-run simulator-clean

# ── Configuration ─────────────────────────────────────────────────────────────

PICO_BOARD ?= pimoroni_pico_plus2_w_rp2350
BUILD_DIR := build
LUA_VERSION := 5.4.7
FATFS_VERSION := R0.15
FATFS_URL := http://elm-chan.org/fsw/ff/arc/ff15.zip

# Lua download URL
LUA_URL := https://www.lua.org/ftp/lua-$(LUA_VERSION).tar.gz

# Third-party directories
LUA_DIR := third_party/lua-5.4
FATFS_DIR := third_party/fatfs

# ── Default target ────────────────────────────────────────────────────────────

help:
	@echo "PicOS Build System"
	@echo ""
	@echo "Hardware Targets:"
	@echo "  make setup          - Download dependencies (Lua, FatFS) and check environment"
	@echo "  make build          - Build the firmware (creates build/picocalc_os.uf2)"
	@echo "  make clean          - Remove build directory"
	@echo "  make rebuild        - Clean and rebuild from scratch"
	@echo "  make flash          - Show instructions for flashing the device"
	@echo "  make check-env      - Verify build environment is ready"
	@echo ""
	@echo "Simulator Targets (PC):"
	@echo "  make simulator      - Build PC simulator for testing/debugging"
	@echo "  make simulator-run  - Build and run the simulator"
	@echo "  make simulator-clean - Clean simulator build files"
	@echo ""
	@echo "Testing:"
	@echo "  make test-lua       - Test Lua app syntax before deployment"
	@echo ""
	@echo "Environment:"
	@echo "  PICO_BOARD          - Target board (default: $(PICO_BOARD))"
	@echo "  PICO_SDK_PATH       - Path to Pico SDK (must be set)"
	@echo ""

# ── Setup target ──────────────────────────────────────────────────────────────

setup: check-env download-lua download-fatfs
	@echo ""
	@echo "✓ Setup complete! Run 'make build' to compile the firmware."

check-env:
	@echo "Checking build environment..."
	@if [ -z "$(PICO_SDK_PATH)" ]; then \
		echo "ERROR: PICO_SDK_PATH environment variable not set."; \
		echo ""; \
		echo "To fix this:"; \
		echo "  1. Clone the Pico SDK:"; \
		echo "     git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk"; \
		echo "     cd ~/pico-sdk && git submodule update --init"; \
		echo ""; \
		echo "  2. Set the environment variable:"; \
		echo "     export PICO_SDK_PATH=~/pico-sdk"; \
		echo "     # Or add to ~/.bashrc for persistence"; \
		echo ""; \
		exit 1; \
	fi
	@if [ ! -d "$(PICO_SDK_PATH)" ]; then \
		echo "ERROR: PICO_SDK_PATH points to non-existent directory: $(PICO_SDK_PATH)"; \
		exit 1; \
	fi
	@if [ ! -f "$(PICO_SDK_PATH)/pico_sdk_init.cmake" ]; then \
		echo "ERROR: $(PICO_SDK_PATH) doesn't look like a valid Pico SDK"; \
		echo "       (missing pico_sdk_init.cmake)"; \
		exit 1; \
	fi
	@echo "  ✓ PICO_SDK_PATH: $(PICO_SDK_PATH)"
	@# Check for ARM toolchain
	@if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then \
		echo "WARNING: arm-none-eabi-gcc not found in PATH"; \
		echo "         Install ARM toolchain:"; \
		echo ""; \
		echo "  Fedora/RHEL:  sudo dnf install gcc-arm-none-eabi newlib-arm-none-eabi"; \
		echo "  Debian/Ubuntu: sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi"; \
		echo "  macOS:        brew install --cask gcc-arm-embedded"; \
		echo ""; \
	else \
		echo "  ✓ arm-none-eabi-gcc: $$(arm-none-eabi-gcc --version | head -n1)"; \
	fi
	@# Check for CMake
	@if ! command -v cmake >/dev/null 2>&1; then \
		echo "ERROR: cmake not found in PATH"; \
		echo "       Install: sudo dnf install cmake  # or apt/brew"; \
		exit 1; \
	else \
		echo "  ✓ cmake: $$(cmake --version | head -n1)"; \
	fi

download-lua:
	@if [ -d "$(LUA_DIR)/src" ] && [ -f "$(LUA_DIR)/src/lua.h" ]; then \
		echo "  ✓ Lua $(LUA_VERSION) already present at $(LUA_DIR)"; \
	else \
		echo "Downloading Lua $(LUA_VERSION)..."; \
		mkdir -p third_party; \
		cd third_party && \
		curl -LO $(LUA_URL) && \
		tar -xzf lua-$(LUA_VERSION).tar.gz && \
		mv lua-$(LUA_VERSION) lua-5.4 && \
		rm lua-$(LUA_VERSION).tar.gz && \
		echo "  ✓ Lua $(LUA_VERSION) extracted to $(LUA_DIR)"; \
	fi

download-fatfs:
	@if [ -d "$(FATFS_DIR)" ] && [ -f "$(FATFS_DIR)/ff.h" ]; then \
		echo "  ✓ FatFS already present at $(FATFS_DIR)"; \
	else \
		echo "Downloading FatFS $(FATFS_VERSION)..."; \
		echo "NOTE: FatFS is pre-configured in third_party/fatfs/"; \
		echo "      If missing, manually download from http://elm-chan.org/fsw/ff/"; \
		echo "      and ensure diskio_spi.c is present in $(FATFS_DIR)/port/"; \
		if [ ! -d "$(FATFS_DIR)" ]; then \
			echo "ERROR: $(FATFS_DIR) not found. See README.md for setup instructions."; \
			exit 1; \
		fi; \
	fi

# ── Build targets ─────────────────────────────────────────────────────────────

build: check-env $(LUA_DIR) $(FATFS_DIR)
	@echo "Building PicOS for $(PICO_BOARD)..."
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && \
		cmake .. -DPICO_BOARD=$(PICO_BOARD) && \
		$(MAKE) -j$$(nproc 2>/dev/null || echo 4)
	@echo ""
	@echo "✓ Build complete!"
	@echo "  Firmware: $(BUILD_DIR)/picocalc_os.uf2"
	@echo ""
	@echo "To flash:"
	@echo "  1. Hold BOOTSEL button on Pico"
	@echo "  2. Connect USB cable"
	@echo "  3. Drag $(BUILD_DIR)/picocalc_os.uf2 to mounted drive"
	@echo ""

clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)
	@echo "✓ Clean complete"

rebuild: clean build

# ── Flash helper ──────────────────────────────────────────────────────────────

flash:
	@if [ ! -f "$(BUILD_DIR)/picocalc_os.uf2" ]; then \
		echo "ERROR: $(BUILD_DIR)/picocalc_os.uf2 not found. Run 'make build' first."; \
		exit 1; \
	fi
	@echo "Flashing instructions:"
	@echo ""
	@echo "  1. Disconnect the Pico from USB"
	@echo "  2. Hold the BOOTSEL button on the Pico"
	@echo "  3. While holding BOOTSEL, connect USB cable"
	@echo "  4. Release BOOTSEL — Pico mounts as USB drive (RPI-RP2)"
	@echo "  5. Copy the firmware:"
	@echo ""
	@echo "     cp $(BUILD_DIR)/picocalc_os.uf2 /path/to/RPI-RP2/"
	@echo ""
	@echo "     Or drag-and-drop in your file manager"
	@echo ""
	@echo "  6. Device will reboot automatically"
	@echo ""
	@# Attempt auto-detection (Linux only)
	@if [ -d "/media/$$USER/RPI-RP2" ]; then \
		echo "✓ Detected RPI-RP2 at /media/$$USER/RPI-RP2"; \
		echo "  Run: cp $(BUILD_DIR)/picocalc_os.uf2 /media/$$USER/RPI-RP2/"; \
		echo ""; \
		read -p "Copy now? [y/N] " confirm && \
		[ "$$confirm" = "y" ] && cp $(BUILD_DIR)/picocalc_os.uf2 /media/$$USER/RPI-RP2/ && \
		echo "✓ Firmware copied! Device will reboot."; \
	fi

# ── Lua App Testing ───────────────────────────────────────────────────────────

test-lua:
	@if ! command -v lua >/dev/null 2>&1; then \
		echo "ERROR: 'lua' interpreter not found in PATH"; \
		echo "       Install: sudo dnf install lua  # or apt/brew"; \
		exit 1; \
	fi
	@echo "Testing Lua app syntax..."
	@lua tools/test_lua_apps.lua

# ── PC Simulator Targets ─────────────────────────────────────────────────────

SIM_BUILD_DIR := build_sim
SIM_BINARY := $(SIM_BUILD_DIR)/picos_simulator

simulator-check:
	@echo "Checking simulator build environment..."
	@if ! command -v cmake >/dev/null 2>&1; then \
		echo "ERROR: cmake not found in PATH"; \
		echo "       Install: sudo dnf install cmake  # or apt/brew"; \
		exit 1; \
	fi
	@if ! pkg-config --exists sdl2 2>/dev/null; then \
		echo "ERROR: SDL2 development libraries not found"; \
		echo ""; \
		echo "       Install SDL2:"; \
		echo "         Fedora:  sudo dnf install SDL2-devel"; \
		echo "         Ubuntu:  sudo apt install libsdl2-dev"; \
		echo "         macOS:   brew install sdl2"; \
		echo ""; \
		exit 1; \
	fi
	@echo "  ✓ cmake: $$(cmake --version | head -n1)"
	@echo "  ✓ SDL2: $$(pkg-config --modversion sdl2)"

simulator: simulator-check download-lua
	@echo "Building PicOS PC Simulator..."
	@mkdir -p $(SIM_BUILD_DIR)
	@if [ -f $(SIM_BUILD_DIR)/CMakeCache.txt ] && \
		! grep -q 'CMAKE_HOME_DIRECTORY.*simulator' $(SIM_BUILD_DIR)/CMakeCache.txt 2>/dev/null; then \
		echo "  Stale CMake cache detected, cleaning..."; \
		rm -rf $(SIM_BUILD_DIR)/*; \
	fi
	@cd $(SIM_BUILD_DIR) && \
		cmake ../simulator -DCMAKE_BUILD_TYPE=Release && \
		$(MAKE) -j$$(nproc 2>/dev/null || echo 4)
	@echo ""
	@echo "✓ Simulator build complete!"
	@echo "  Binary: $(SIM_BINARY)"
	@echo ""
	@echo "To run:"
	@echo "  $(SIM_BINARY) --sd-card ./apps"
	@echo ""
	@echo "For help:"
	@echo "  $(SIM_BINARY) --help"
	@echo ""

simulator-debug: simulator-check download-lua
	@echo "Building PicOS PC Simulator (Debug)..."
	@mkdir -p $(SIM_BUILD_DIR)
	@if [ -f $(SIM_BUILD_DIR)/CMakeCache.txt ] && \
		! grep -q 'CMAKE_HOME_DIRECTORY.*simulator' $(SIM_BUILD_DIR)/CMakeCache.txt 2>/dev/null; then \
		echo "  Stale CMake cache detected, cleaning..."; \
		rm -rf $(SIM_BUILD_DIR)/*; \
	fi
	@cd $(SIM_BUILD_DIR) && \
		cmake ../simulator -DCMAKE_BUILD_TYPE=Debug && \
		$(MAKE) -j$$(nproc 2>/dev/null || echo 4)
	@echo ""
	@echo "✓ Simulator build complete (Debug)!"
	@echo "  Binary: $(SIM_BINARY)"
	@echo ""

simulator-run: simulator
	@echo "Running PicOS Simulator..."
	@echo ""
	@$(SIM_BINARY) --sd-card ./apps

simulator-clean:
	@echo "Cleaning simulator build..."
	@rm -rf $(SIM_BUILD_DIR)
	@echo "✓ Simulator clean complete"
