# PicOS PC Simulator

A cross-platform SDL2-based simulator for PicOS that runs on desktop computers for development, debugging, and testing.

## Features

- **3x Scale Display**: 960×960 window simulating the 320×320 LCD
- **Dual-Core Simulation**: Core 0 (Lua VM) and Core 1 (audio/network) run in separate threads
- **Keyboard Input**: Full keyboard mapping to PicOS buttons
- **Host Filesystem**: SD card simulated as local directory
- **Audio Support**: SDL2 audio output
- **Debuggable**: Use GDB/LLDB, printf debugging, IDE breakpoints

## Building

### Prerequisites

- CMake 3.13+
- SDL2 development libraries
- C/C++ compiler (GCC, Clang, MSVC)

**Fedora/Linux:**
```bash
sudo dnf install cmake SDL2-devel
```

**macOS:**
```bash
brew install cmake sdl2
```

**Windows (MSYS2):**
```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-SDL2
```

### Build Commands

```bash
# Create build directory
mkdir build_sim && cd build_sim

# Configure
cmake .. -DPICOS_SIMULATOR=ON

# Build
make -j4

# Run
./picos_simulator
```

## Usage

```bash
# Run with default SD card path (.)
./picos_simulator

# Run with custom SD card path
./picos_simulator --sd-card /path/to/apps

# Show boot splash screen with delays (for screencasting)
./picos_simulator --show-splash

# Enable debug logging
./picos_simulator --debug

# Show help
./picos_simulator --help
```

## Keyboard Mapping

| PC Key | PicOS Button |
|--------|-------------|
| Arrow Keys | BTN_UP/DOWN/LEFT/RIGHT |
| Enter | BTN_ENTER |
| Escape | BTN_ESC |
| F1-F4 | BTN_F1-F4 |
| Tab | BTN_TAB |
| Backspace | BTN_BACKSPACE |
| Delete | BTN_DEL |
| Home | BTN_FN |
| Ctrl | BTN_CTRL |
| Shift | BTN_SHIFT |

## Project Structure

```
simulator/
├── CMakeLists.txt          # Simulator build configuration
├── main.c                  # SDL2 main loop, dual-core simulation
├── hal/                    # Hardware Abstraction Layer
│   ├── hal_display.c/h     # SDL2 320×320 display
│   ├── hal_input.c/h       # Keyboard → button mapping
│   ├── hal_sdcard.c/h      # Host filesystem
│   ├── hal_psram.c/h       # Host memory allocation
│   ├── hal_timing.c/h      # SDL2 timing
│   ├── hal_audio.c/h       # SDL2 audio
│   └── hal_threading.c/h   # POSIX threads
├── stubs/                  # Pico SDK stubs
│   ├── pico_sdk_stubs.c/h  # Minimal SDK implementations
│   └── hardware_stubs.c/h  # Hardware register stubs
└── assets/
    └── sd_card/            # Simulated SD card
        ├── apps/           # Lua apps
        ├── data/           # App data
        └── system/         # System config
```

## Differences from Hardware

1. **No actual GPIO**: Hardware drivers are stubbed
2. **No WiFi**: Network stack stubbed (can be enabled with host sockets)
3. **Faster execution**: PC is faster than RP2350
4. **More memory**: Uses host RAM instead of limited PSRAM
5. **Different timing**: SDL2 timing vs hardware timers

## Adding to CI/CD

The simulator enables automated testing:

```yaml
# GitHub Actions example
- name: Build Simulator
  run: |
    mkdir build_sim && cd build_sim
    cmake .. -DPICOS_SIMULATOR=ON
    make -j4

- name: Run Tests
  run: |
    ./build_sim/picos_simulator --sd-card ./test_apps &
    sleep 5
    # Run automated tests...
```

## Future Enhancements

- [ ] GDB remote debugging support
- [ ] Screenshot/video capture for regression testing
- [ ] Network passthrough to host
- [ ] Performance profiling tools
- [ ] Headless mode for CI

## License

Same as PicOS (see main project LICENSE)
