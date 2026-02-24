# PicOS

An app platform for the ClockworkPi PicoCalc, built around a resident Lua runtime. Apps live on the SD card as directories containing `main.lua` and `app.json`. The OS owns all hardware; apps access everything through the `picocalc` Lua API.

📖 **Documentation:** Full SDK reference and guides are available on the [PicOS Wiki](https://github.com/jeffory/PicOS/wiki).

**Target hardware:** [Pimoroni Pico Plus 2 W](https://shop.pimoroni.com/products/pimoroni-pico-plus-2-w) in the [PicoCalc](https://www.clockworkpi.com/picocalc).

Other hardware support is untested currently.

---

## Architecture

```
Flash & Memory
├── OS kernel + drivers (Double-buffered 60 FPS tear-free display via 100MHz SPI)
├── Lua 5.4 runtime (Resident 6MB heap securely mapped into PSRAM)
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
local pc = picocalc

-- Display
pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 20, "Hello!", pc.display.WHITE, pc.display.BLACK)
pc.display.fillRect(50, 50, 100, 100, pc.display.rgb(255, 0, 0))
pc.display.flush()

-- Input
pc.input.update()                -- poll keyboard (call once per frame)
local btns = pc.input.getButtonsPressed()
if btns & pc.input.BTN_ENTER ~= 0 then ... end
local ch = pc.input.getChar()   -- typed character or nil

-- UI (standard header/footer bars)
pc.ui.drawHeader("My App")
pc.ui.drawFooter("Press Esc to exit", "Bat: 85%")

-- Graphics (image loading)
local img = pc.graphics.image.load("/apps/myapp/sprite.bmp")
img:draw(100, 100)

-- System
pc.sys.sleep(16)                 -- sleep ms (~60fps)
pc.sys.log("debug message")
local bat = pc.sys.getBattery()  -- 0-100

-- Filesystem (SD card)
local data = pc.fs.readFile("/data/mygame/save.json")
```

---

## Hardware Pin Reference
All pins are defined in `src/hardware.h`. Verify against [clockwork_Mainboard_V2.0_Schematic.pdf](https://github.com/clockworkpi/PicoCalc/blob/master/clockwork_Mainboard_V2.0_Schematic.pdf).

| Peripheral | Interface | Pins |
|---|---|---|
| ST7365P LCD | PIO SPI (pio0) | MOSI=11, SCK=10, CS=13, DC=14, RST=15 |
| SD Card | SPI0 | MOSI=19, SCK=18, MISO=16, CS=17 |
| Keyboard (STM32) | I2C1 | SDA=6, SCL=7, addr=0x1F |
| Audio L/R | PWM | GP26 (L), GP27 (R) |

**Note:** Backlight is controlled by the STM32 keyboard MCU over I2C, not a direct GPIO. Use `picocalc.display.setBrightness()` in Lua or `kbd_set_backlight()` in C.

**Note:** The LCD uses a dedicated PIO SPI master, freeing up hardware SPI1 entirely. WiFi (CYW43 on Pico 2W) uses its own internal bus.

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
- `make test-lua` — Test all Lua apps for syntax errors before deployment
- `make help` — Show all targets

---

## Testing Lua Apps

Before copying apps to the SD Card, it's recommended to validate them first:

```bash
make test-lua
```

The test tool (`tools/test_lua_apps.lua`) checks:
- **Syntax correctness** — missing `end`, unmatched quotes, invalid operators
- **API compatibility** — validates against mock `picocalc` API
- **All apps** in `apps/` directory

Example output:
```
=== PicOS Lua App Syntax Checker ===

[✓] apps/hello/main.lua
[✓] apps/snake/main.lua
[✗] apps/editor/main.lua
  apps/editor/main.lua:42: ')' expected near 'end'

=== Summary ===
Passed: 2
Failed: 1
```

Catches common errors before deployment:
- Missing button constants (`BTN_CTRL`, `BTN_BACKSPACE`)
- Unclosed functions, loops, or conditionals
- String/syntax errors

**Note:** `make build` automatically runs tests. To skip: `make build -o test-lua`

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
local pc = picocalc

-- Your app runs in a plain while loop
-- Return (or fall off the end) to go back to the launcher

while true do
    pc.perf.beginFrame()
    pc.input.update()  -- poll keyboard (call once per frame)

    local pressed = pc.input.getButtonsPressed()
    if pressed & pc.input.BTN_ESC ~= 0 then return end

    pc.display.clear(pc.display.BLACK)
    pc.ui.drawHeader("My App!")
    pc.display.drawText(10, 150, "Hello!", pc.display.WHITE, pc.display.BLACK)
    pc.ui.drawFooter("Press Esc to exit")
    pc.perf.drawFPS()
    pc.display.flush()

    pc.perf.endFrame()
end
```

### Full Lua API reference

#### `picocalc.display`
| Function | Description |
|---|---|
| `clear([color])` | Fill screen with colour (default: BLACK) |
| `setPixel(x, y, color)` | Draw a single pixel |
| `fillRect(x, y, w, h, color)` | Filled rectangle |
| `drawRect(x, y, w, h, color)` | Outline rectangle |
| `drawLine(x0, y0, x1, y1, color)` | Bresenham line |
| `drawText(x, y, text, fg [, bg])` | 6×8 pixel font, returns pixel width |
| `textWidth(text)` | Calculate pixel width without drawing |
| `flush()` | Push framebuffer to LCD (call once per frame) |
| `getWidth()` | Returns 320 |
| `getHeight()` | Returns 320 |
| `setBrightness(0-255)` | Backlight level |
| `rgb(r, g, b)` | Create RGB565 colour from components |
| Constants | `BLACK, WHITE, RED, GREEN, BLUE, YELLOW, CYAN, GRAY` |

#### `picocalc.input`
| Function | Description |
|---|---|
| `update()` | Poll keyboard — call once per frame |
| `getButtons()` | Bitmask of currently held buttons |
| `getButtonsPressed()` | Bitmask of buttons pressed this frame |
| `getButtonsReleased()` | Bitmask of buttons released this frame |
| `getChar()` | Last typed character (string) or nil |
| `getRawKey()` | Raw STM32 keycode |
| Constants | `BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_ENTER, BTN_ESC, BTN_MENU, BTN_F1-F9, BTN_BACKSPACE, BTN_TAB, BTN_DEL, BTN_SHIFT, BTN_CTRL, BTN_ALT, BTN_FN` |

#### `picocalc.sys`
| Function | Description |
|---|---|
| `getTimeMs()` | Milliseconds since boot |
| `getBattery()` | Battery % (0-100) or -1 |
| `getClock()` | Returns `{synced, hour, min, sec, epoch}` |
| `sleep(ms)` | Sleep for ms milliseconds |
| `log(msg)` | Print to USB serial debug |
| `exit()` | Exit app cleanly, return to launcher |
| `reboot()` | System reboot |
| `isUSBPowered()` | Check if USB powered |
| `addMenuItem(label, fn)` | Add item to system menu (max 4) |
| `clearMenuItems()` | Remove app menu items |

#### `picocalc.fs`
| Function | Description |
|---|---|
| `readFile(path)` | Read entire file, returns string or nil |
| `open(path, mode)` | Open file ("r", "w", "a") |
| `read(f, len)` | Read len bytes |
| `write(f, data)` | Write string |
| `close(f)` | Close file handle |
| `seek(f, pos)` | Seek to byte position |
| `tell(f)` | Get current byte position |
| `exists(path)` | Returns bool |
| `size(path)` | File size in bytes |
| `listDir(path)` | Array of `{name, is_dir, size}` entries |
| `mkdir(path)` | Create directory |
| `appPath(name)` | Returns `/data/<appname>/<name>`, auto-creates dir |
| `browse([startDir])` | File browser overlay, returns selected path or nil |

#### `picocalc.graphics`
| Function | Description |
|---|---|
| `setColor(color)` | Set current drawing color |
| `setBackgroundColor(color)` | Set background color |
| `clear([color])` | Clear using background color |
| `image.new(w, h)` | Create blank image in PSRAM |
| `image.load(path)` | Load BMP/JPEG/PNG/GIF from SD card |
| `image.loadFromBuffer(data)` | Decode image from memory buffer |
| `img:draw(x, y, [opts], [srcRect])` | Draw with optional flip/sub-rect |
| `img:drawAnchored(x, y, ax, ay)` | Draw with anchor point (0.0-1.0) |
| `img:drawTiled(x, y, w, h)` | Tile image across region |
| `img:drawScaled(x, y, scale, [angle])` | Draw scaled/rotated |
| `img:getSize()` | Returns width, height |
| `img:copy()` | Deep-copy an image |

#### `picocalc.ui`
| Function | Description |
|---|---|
| `drawHeader(title)` | Draw standard header bar |
| `drawFooter([left], [right])` | Draw standard footer bar |

#### `picocalc.network`
| Function | Description |
|---|---|
| `getStatus()` | Network status (`kStatusNotConnected`, `kStatusConnected`, `kStatusNotAvailable`) |
| `setEnabled(flag, [callback])` | Enable/disable WiFi |
| `http.new(server, [port])` | Create HTTP connection object |
| `conn:get(path, [headers])` | HTTP GET request |
| `conn:post(path, [headers], data)` | HTTP POST request |
| `conn:read([length])` | Read response data |
| `conn:close()` | Close connection |

#### `picocalc.config`
| Function | Description |
|---|---|
| `get(key)` | Get config value (string or nil) |
| `set(key, value)` | Set config value (nil to delete) |
| `save()` | Persist to `/system/config.json` |
| `load()` | Reload from disk |

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

- [x] WiFi API (`picocalc.wifi.connect`, `getStatus`, etc.)
- [x] System menu overlay (triggered by Menu key, pauses app)
- [x] Shared config (WiFi credentials, brightness persisted to `/system/config.json`)
- [x] Image loading from SD card (BMP, JPEG, PNG, GIF via `picocalc.graphics.image.load()`)
- [x] App data directory helpers (`pc.fs.appPath("save.dat")` → `/data/appname/save.dat`)
- [x] HTTP client (`picocalc.network.http.*`) with async callbacks
- [x] Performance monitoring (`picocalc.perf.*`)
- [x] Standard UI components (`picocalc.ui.drawHeader/drawFooter`)
- [ ] Audio API (`picocalc.audio.playTone`)
- [ ] Native C app loader (binary relocation into PSRAM for performance-critical apps)

---

## Project Structure

```
picocalc-os/
├── CMakeLists.txt
├── pico_sdk_import.cmake      ← copy from $PICO_SDK_PATH/external/
├── README.md
├── SDK.md                     ← full Lua API reference
├── src/
│   ├── hardware.h             ← ALL pin definitions here
│   ├── main.c                 ← boot, init, splash, calls launcher_run()
│   ├── os/
│   │   ├── os.h               ← PicoCalcAPI struct (hardware API table)
│   │   ├── launcher.h/c       ← app discovery + scrollable menu
│   │   ├── lua_bridge.h/c     ← C functions registered as picocalc.* Lua API
│   │   ├── lua_psram_alloc.h/c ← PSRAM-backed Lua heap allocator
│   │   ├── system_menu.h/c    ← system menu overlay (Menu key)
│   │   ├── config.h/c         ← persistent JSON config
│   │   ├── clock.h/c          ← NTP time sync
│   │   ├── ui.h/c             ← shared header/footer drawing
│   │   ├── image_decoders.h/cpp ← JPEG/PNG/GIF decoding
│   │   ├── file_browser.h/c   ← file picker overlay
│   │   ├── screenshot.h/c     ← BMP screenshot capture
│   │   └── text_input.h/c     ← text entry overlay
│   └── drivers/
│       ├── display.h/c        ← ST7365P, PIO SPI, DMA double-buffered flush
│       ├── keyboard.h/c       ← STM32 I2C keyboard + battery
│       ├── sdcard.h/c         ← FatFS wrapper
│       ├── wifi.h/c           ← CYW43 WiFi driver
│       └── http.h/c           ← Mongoose HTTP client
├── apps/
│   ├── hello/                 ← example: drawing + input
│   ├── tetris/                ← example: full game
│   ├── snake/                 ← example: full game
│   ├── wikipedia/             ← example: networking
│   └── image_viewer/          ← example: graphics API
└── third_party/
    ├── lua-5.4/               ← Lua 5.4 source
    ├── fatfs/                 ← Chan FatFS + SPI port
    ├── umm_malloc/            ← Embedded memory allocator for PSRAM Lua heap
    ├── mongoose/              ← Mongoose networking library
    ├── tgx/                   ← TGX graphics library (scaling/rotation)
    ├── JPEGDEC/               ← JPEG decoder
    ├── PNGdec/                ← PNG decoder
    └── AnimatedGIF/           ← GIF decoder
```
