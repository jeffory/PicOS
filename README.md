# PicoOS

An app platform for the ClockworkPi PicoCalc, built around a resident Lua runtime. Apps live on the SD card as directories containing `main.lua` and `app.json`. The OS owns all hardware; apps access everything through the `picocalc` Lua API.

**Target hardware:** Pimoroni Pico Plus 2 W in PicoCalc v2.0 mainboard  
*(Standard Pico 2 works too — just without PSRAM for the framebuffer and no WiFi)*

---

## Architecture

```
Flash (resident, never replaced)
├── OS kernel + drivers
├── Lua 5.4 runtime
└── Launcher (app menu)

SD Card (hot-swappable)
└── /apps/
    ├── hello/
    │   ├── app.json      ← name, description, version
    │   └── main.lua      ← your app
    └── snake/
        ├── app.json
        └── main.lua
└── /data/               ← app save files
└── /system/             ← OS config (WiFi credentials, brightness, etc.)
```

The `picocalc` Lua global is your entire interface to the hardware:

```lua
local pd = picocalc

-- Display
pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 20, "Hello!", pc.display.WHITE, pc.display.BLACK)
pc.display.fillRect(50, 50, 100, 100, pc.display.rgb(255, 0, 0))
pc.display.flush()

-- Input
local btns = pc.input.getButtonsPressed()
if btns & pc.input.BTN_ENTER ~= 0 then ... end
local ch = pc.input.getChar()   -- typed character or nil

-- System
pc.sys.sleep(16)                 -- sleep ms (~60fps)
pc.sys.log("debug message")
local bat = pc.sys.getBattery()  -- 0-100

-- Filesystem (SD card)
local data = pc.fs.readFile("/data/mygame/save.json")
```

---

## Hardware Pin Reference

All pins are defined in `src/hardware.h`. Verify against `clockwork_Mainboard_V2.0_Schematic.pdf`.

| Peripheral | Interface | Pins |
|---|---|---|
| ST7365P LCD | SPI1 | MOSI=11, SCK=10, CS=13, DC=14, RST=15 |
| SD Card | SPI0 | MOSI=19, SCK=18, MISO=16, CS=17 |
| Keyboard (STM32) | I2C1 | SDA=6, SCL=7, addr=0x1F |
| Audio L/R | PWM | GP26 (L), GP27 (R) |

**Note:** Backlight is controlled by the STM32 keyboard MCU over I2C, not a direct GPIO. Use `picocalc.display.setBrightness()` in Lua or `kbd_set_backlight()` in C.

**Note:** SPI1 is shared between the LCD and WiFi (CYW43 on Pico 2W). The display driver uses a mutex to arbitrate access.

---

## Build Setup

**Quick Start (recommended):**

```bash
# Install ARM toolchain
sudo dnf install cmake gcc-arm-none-eabi newlib-arm-none-eabi  # Fedora
# sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi  # Debian/Ubuntu

# Get Pico SDK
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
export PICO_SDK_PATH=~/pico-sdk

# Setup dependencies and build
make setup
make build
```

Output: `build/picocalc_os.uf2` — drag-and-drop to Pico in BOOTSEL mode.

**Available Make targets:**
- `make setup` — Download Lua/FatFS, verify environment
- `make build` — Compile firmware
- `make clean` — Remove build directory
- `make rebuild` — Clean and rebuild
- `make flash` — Show flashing instructions (auto-detects RPI-RP2 on Linux)
- `make help` — Show all targets

---

### Manual Setup

<details>
<summary>Click to expand manual build instructions</summary>

### 1. Install prerequisites

```bash
# Fedora
sudo dnf install cmake gcc-arm-none-eabi-cs gcc-arm-none-eabi-cs-c++ newlib-arm-none-eabi

# Or use the Raspberry Pi Pico toolchain installer:
# https://github.com/raspberrypi/pico-setup-windows (Windows)
# https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf (Linux)
```

### 2. Get the Pico SDK

```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
echo 'export PICO_SDK_PATH=~/pico-sdk' >> ~/.bashrc
source ~/.bashrc
```

### 3. Copy `pico_sdk_import.cmake`

```bash
cp $PICO_SDK_PATH/external/pico_sdk_import.cmake .
```

Lua 5.4 and FatFS (including the RP2350 SPI port) are already vendored in `third_party/` — no download needed.

### 4. Build

```bash
mkdir build && cd build
cmake .. -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
make -j4
```

This produces `build/picocalc_os.uf2`. Flash it by holding BOOTSEL on your Pico while plugging in USB, then drag the UF2 to the mounted drive.

Other board values: `pico2` (no PSRAM/WiFi), `pico_w`, `pico`.

</details>

---

## Prepare your SD card

Format the SD card as FAT32. Copy the `apps/` folder to the root:

```
SD:/
├── apps/
│   ├── hello/
│   │   ├── app.json
│   │   └── main.lua
│   └── snake/
│       ├── app.json
│       └── main.lua
├── data/     (created automatically)
└── system/   (created automatically)
```

---

## Writing Apps

Every app is a directory in `/apps/` containing at minimum:

**`app.json`**
```json
{
    "name": "My App",
    "description": "What it does",
    "version": "1.0"
}
```

**`main.lua`**
```lua
local pd = picocalc

-- Your app runs in a plain while loop
-- Return (or fall off the end) to go back to the launcher

while true do
    local pressed = pc.input.getButtonsPressed()
    if pressed & pc.input.BTN_ESC ~= 0 then return end

    pc.display.clear(pc.display.BLACK)
    pc.display.drawText(10, 150, "My App!", pc.display.WHITE, pc.display.BLACK)
    pc.display.flush()

    pc.sys.sleep(16)  -- aim for ~60fps
end
```

### Full Lua API reference

#### `picocalc.display`
| Function | Description |
|---|---|
| `clear(color)` | Fill screen with colour |
| `setPixel(x, y, color)` | Draw a single pixel |
| `fillRect(x, y, w, h, color)` | Filled rectangle |
| `drawRect(x, y, w, h, color)` | Outline rectangle |
| `drawLine(x0, y0, x1, y1, color)` | Bresenham line |
| `drawText(x, y, text, fg, bg)` | 6×8 pixel font, returns pixel width |
| `flush()` | Push framebuffer to LCD (call once per frame) |
| `getWidth()` | Returns 320 |
| `getHeight()` | Returns 320 |
| `setBrightness(0-255)` | Backlight level |
| `rgb(r, g, b)` | Create RGB565 colour from components |
| Constants | `BLACK, WHITE, RED, GREEN, BLUE, YELLOW, CYAN, GRAY` |

#### `picocalc.input`
| Function | Description |
|---|---|
| `getButtons()` | Bitmask of currently held buttons |
| `getButtonsPressed()` | Bitmask of buttons pressed this frame |
| `getButtonsReleased()` | Bitmask of buttons released this frame |
| `getChar()` | Last typed character (string) or nil |
| Constants | `BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_ENTER, BTN_ESC, BTN_MENU` |

#### `picocalc.sys`
| Function | Description |
|---|---|
| `getTimeMs()` | Milliseconds since boot |
| `getBattery()` | Battery % (0-100) or -1 |
| `sleep(ms)` | Sleep for ms milliseconds |
| `log(msg)` | Print to USB serial debug |

#### `picocalc.fs`
| Function | Description |
|---|---|
| `readFile(path)` | Read entire file, returns string or nil |
| `open(path, mode)` | Open file ("r", "w", "a") |
| `read(f, len)` | Read len bytes |
| `write(f, data)` | Write string |
| `close(f)` | Close file handle |
| `exists(path)` | Returns bool |

#### `picocalc.perf`
| Function | Description |
|---|---|
| `beginFrame()` | Start timing a frame (call at loop start) |
| `endFrame()` | End timing and calculate FPS (call at loop end) |
| `getFPS()` | Get current FPS (averaged over 30 frames) |
| `getFrameTime()` | Get last frame time in milliseconds |
| `drawFPS([x, y])` | Draw color-coded FPS counter (default: 250, 8) |

**Example:**
```lua
while true do
    pc.perf.beginFrame()
    -- ... your rendering code ...
    pc.perf.drawFPS()  -- Easy!
    pc.display.flush()
    pc.perf.endFrame()
    pc.sys.sleep(16)
end
```

---

## Roadmap

- [ ] WiFi API (`picocalc.wifi.connect`, `getStatus`, etc.)
- [ ] Audio API (`picocalc.audio.playTone`)  
- [ ] System menu overlay (triggered by Menu key, pauses app)
- [ ] Shared config (WiFi credentials, brightness persisted to `/system/config.json`)
- [ ] Sprite / bitmap loading from SD card
- [ ] `picocalc.display.drawBitmap(x, y, path)` for BMP/raw image files
- [ ] App data directory helpers (`pc.fs.appPath("save.dat")` → `/data/appname/save.dat`)
- [ ] Native C app loader (binary relocation into PSRAM for performance-critical apps)

---

## Project Structure

```
picocalc-os/
├── CMakeLists.txt
├── pico_sdk_import.cmake      ← copy from $PICO_SDK_PATH/external/
├── README.md
├── src/
│   ├── hardware.h             ← ALL pin definitions here
│   ├── main.c                 ← boot, init, splash, calls launcher_run()
│   ├── os/
│   │   ├── os.h               ← PicoCalcAPI struct (hardware API table)
│   │   ├── launcher.h/c       ← app discovery + scrollable menu
│   │   └── lua_bridge.h/c     ← C functions registered as picocalc.* Lua API
│   └── drivers/
│       ├── display.h/c        ← ST7365P, DMA framebuffer flush
│       ├── keyboard.h/c       ← STM32 I2C keyboard + battery
│       ├── sdcard.h/c         ← FatFS wrapper
│       └── audio.h/c          ← PWM audio (TODO)
├── apps/
│   ├── hello/                 ← example: drawing + input
│   └── snake/                 ← example: full game
└── third_party/
    ├── lua-5.4/               ← Lua 5.4 source
    └── fatfs/                 ← Chan FatFS + SPI port
```
