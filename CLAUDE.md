# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PicOS is a bare-metal embedded OS for the [ClockworkPi PicoCalc v2.0](https://www.clockworkpi.com/) handheld device. It runs on a Pimoroni Pico Plus 2 W (RP2350, 8MB QMI PSRAM + 8MB PIO PSRAM, WiFi). A resident C firmware lives in flash; apps are Lua scripts or native ELF binaries on an SD card; all hardware is exposed via a `picocalc.*` Lua API and a C `PicoCalcAPI` struct.

## Build Commands

### Prerequisites

**Fedora/Linux:**
```bash
sudo dnf install cmake gcc-arm-none-eabi-cs gcc-arm-none-eabi-cs-c++ newlib-arm-none-eabi
```

**macOS (one-shot):**
```bash
chmod +x setup.sh && ./setup.sh
```

**Pico SDK (required for all platforms):**
```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
export PICO_SDK_PATH=~/pico-sdk
```

### Build
```bash
mkdir build && cd build
cmake .. -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350
make -j4
```

Other board values: `pico2` (no PSRAM/WiFi), `pico_w`, `pico`. Note: the board name in `CMakeLists.txt` defaults to `pimoroni_pico_plus2_w_rp2350` (full SDK name — not `pimoroni_pico_plus2_w`).

Output: `build/picocalc_os.uf2` — drag-and-drop to Pico in BOOTSEL mode.

### Debug
USB serial at 115200 baud. App log calls appear as `[APP] message`. Lua errors display on-screen with stack trace (dismiss with **Esc**).

There is no automated test suite or linter.

## Architecture

### Boot Sequence (`src/main.c`)
```
main()
  set_sys_clock_khz(200000)     // 200 MHz overclock
  Wire g_api struct (function pointers)
  display_init()                // PIO0 SPI, double-buffered SRAM
  pio_psram_init()              // Mainboard 8MB PIO PSRAM (non-fatal if absent)
  sound_init() → audio_init()
  kbd_init()
  sdcard_init()
  lua_psram_alloc_init()        // umm_malloc heap on QMI PSRAM
  config_load()                 // /system/config.json
  wifi_init() → http_init() → tcp_init()
  multicore_launch_core1()      // Core 1: wifi_poll, http, mp3, fileplayer
  system_menu_init()
  launcher_run()                // Never returns
```

### Dual-Core Model
- **Core 0**: Runs the launcher, Lua VM, and native apps. Owns the display, keyboard, and SD card.
- **Core 1**: Runs the network stack (Mongoose/CYW43) and audio decode (MP3, fileplayer). Polls every 5ms. Core 0 communicates via IPC ring buffer (`wifi_req_push()`). **Core 0 must never call `mg_*` functions directly.**

### Central API (`src/os/os.h`)
`PicoCalcAPI g_api` is a function pointer table wired in `main.c`. Sub-tables (all available to both Lua and native C apps unless noted):
- `picocalc.display` / `g_api.display` — drawing primitives, DMA flush, brightness, `drawImageNN`, `flushRows`
- `picocalc.input` / `g_api.input` — button state, character input, edge detection
- `picocalc.fs` / `g_api.fs` — file open/read/write/close/exists/size/listDir
- `picocalc.sys` / `g_api.sys` — time, battery, log, reboot, system menu, poll (native apps), shouldExit
- `picocalc.wifi` / `g_api.wifi` — connect/disconnect/status/IP/SSID/isAvailable
- `picocalc.config` / `g_api.config` — system-wide key/value config (get/set/save/load)
- `picocalc.audio` / `g_api.audio` — tone generation, PCM streaming (playTone/stopTone/setVolume/startStream/stopStream/pushSamples)
- `picocalc.tcp` / `g_api.tcp` — raw TCP/TLS sockets (connect/write/read/close/available/getError/getEvents)
- `picocalc.ui` / `g_api.ui` — modal dialogs (textInput/textInputSimple/confirm)
- `g_api.http` — HTTP/HTTPS client (Phase 1; Lua exposes as `picocalc.network.http` OO objects)
- `g_api.soundplayer` — sample/fileplayer/MP3 player (Phase 1; Lua exposes as `picocalc.sound`)
- `g_api.appconfig` — per-app key/value config (Phase 1; Lua exposes as `picocalc.appconfig`)
- `g_api.crypto` — crypto primitives: SHA-256/SHA-1/HMAC/AES-CTR/ECDH (Phase 1; Lua exposes as `picocalc.crypto`)
- `g_api.graphics` — image loading and drawing (Phase 2; Lua exposes as `picocalc.graphics.image`)
- `g_api.video` — MJPEG video playback (Phase 2; Lua exposes as `picocalc.video`)
- `g_api.version` — 1 = Phase 1 additions present, 2 = Phase 2 additions present

### App Lifecycle (`src/os/launcher.c`)
1. Scans `/apps/` on SD for dirs containing `main.lua` or `main.elf`
2. Reads `app.json` with a hand-rolled minimal JSON parser (no cJSON — flash savings)
3. Auto-detects app type: `main.elf` → native, `main.lua` → Lua (native wins if both present)
4. Shows scrollable menu with battery % header
5. Dispatches via `AppRunner` vtable (`src/os/app_runner.h`):
   - **Lua apps** (`src/os/lua_runner.c`): `malloc`s `main.lua`, creates fresh `lua_State`, calls `lua_bridge_register()`, runs `lua_pcall()`, then `lua_close()`
   - **Native apps** (`src/os/native_loader.c`): ELF32 PIE loader, relocates to PSRAM (code in SRAM if fits), runs on PSP (Process Stack Pointer) via `launch_on_psp()` trampoline

### Lua Bridge (split across `src/os/lua_bridge_*.c`)
The Lua bridge is split into 14 module files, coordinated by `lua_bridge.c`:
- `lua_bridge_audio.c` — tone/PCM streaming
- `lua_bridge_config.c` — config get/set/save/load
- `lua_bridge_display.c` — drawing primitives
- `lua_bridge_fs.c` — filesystem operations
- `lua_bridge_graphics.c` — image loading, sprites, spritesheets, animations, image cache
- `lua_bridge_input.c` — buttons, keyboard
- `lua_bridge_network.c` — WiFi control, HTTP client (OO connections with `HTTP_MT` metatable)
- `lua_bridge_perf.c` — performance profiling
- `lua_bridge_repl.c` — interactive Lua REPL
- `lua_bridge_sound.c` — sound samples, file player, MP3 player
- `lua_bridge_sys.c` — system functions, menu items
- `lua_bridge_ui.c` — modal dialogs
- `lua_bridge_video.c` — MJPEG video playback

All `picocalc.*` Lua functions are `static int l_<module>_<fn>(lua_State *L)` wrappers. Registered via `luaL_Reg` tables passed to `register_subtable()`. Integer constants (button codes, color names) are pushed with `lua_pushinteger` / `lua_setfield`.

Lua 5.4.7 is embedded with restricted stdlib: `base`, `table`, `string`, `math`, `utf8`, `coroutine`. Blocked: `io`, `os`, `package`. Compile-time config: `LUA_32BITS=1`, `LUA_USE_LONGJMP=1`, `LUAI_MAXSTACK=500`.

A debug hook fires every 256 opcodes (`lua_sethook` with `LUA_MASKCOUNT`). The hook checks for the Sym (Menu) key and fires pending HTTP Lua callbacks via `http_lua_fire_pending()`.

### Display Driver (`src/drivers/display.c`)
- ST7365P 320×320 IPS LCD over **PIO0 SPI** at 100 MHz (not hardware SPI1)
- Double-buffered: `uint16_t s_framebuffers[2][320*320]` in **SRAM** (not PSRAM)
- Non-blocking DMA flush via `display_flush()` — DMA reads SRAM (AHB bus), independent of PSRAM (QMI/XIP)
- PIO0 is **completely independent** from the CYW43 WiFi chip (which uses SPI1) — no bus sharing, no lock needed
- Colors are RGB565 host-byte-order; display.c byte-swaps on write (display is big-endian)
- Built-in 6×8 bitmap font for ASCII 0x20–0x7E
- `display_darken()` copies front buffer → back buffer (darkened) for system menu overlay

### WiFi / Network Stack (`src/drivers/wifi.c`, `http.c`, `tcp.c`)
- CYW43 on SPI1; `WIFI_ENABLED=1` defined by CMake for WiFi boards; all CYW43 code is `#ifdef WIFI_ENABLED` guarded
- **Core 1 exclusively owns the Mongoose event manager** (`s_mgr`). Core 0 never calls `mg_*` functions.
- Core 0 → Core 1 IPC: spinlock-guarded 8-slot ring buffer; push via `wifi_req_push()`. Request types: `CONN_REQ_HTTP_START`, `CONN_REQ_HTTP_CLOSE`, `CONN_REQ_WIFI_CONNECT`, `CONN_REQ_WIFI_DISCONNECT`
- Auto-connects on boot if `"wifi_ssid"` / `"wifi_pass"` exist in config

### HTTP Client (`src/drivers/http.c`)
- Mongoose-based HTTP/1.1 client running on Core 1
- Static pool of 8 simultaneous connections (`HTTP_MAX_CONNECTIONS`)
- **HTTPS supported** via `pico_lwip_mbedtls` + `pico_mbedtls` (see `src/mbedtls_config.h`)
- `pending` bitmask set by Core 1 (`http_ev_fn`); Lua callbacks fired by Core 0 via `http_lua_fire_pending()`
- `http_close_all()` called at start of `lua_bridge_register()` to clear stale connections between apps

### TCP Sockets (`src/drivers/tcp.c`)
- Raw TCP/TLS client layer, also Mongoose-based on Core 1
- Pool of 4 connections (`TCP_MAX_CONNECTIONS`)
- Non-blocking, cross-core (Core 0 requests, Core 1 handles)
- Exposed as `picocalc.tcp` in the C API and via Lua bridge

### Audio (`src/drivers/audio.c`, `sound.c`, `mp3_player.c`, `fileplayer.c`)
- PWM audio output on GP26 (left) / GP27 (right)
- `audio.c` — tone generation (square wave) and PCM streaming via DMA
- `sound.c` — sound sample loading and playback (WAV-like)
- `fileplayer.c` — streaming file playback from SD card
- `mp3_player.c` — MP3 decoding via libmad; PCM ring buffer (32KB) in PIO PSRAM; DMA ISR reads from 1KB SRAM staging buffer, refilled by Core 1
- `mp3_player_update()` and `fileplayer_update()` called on Core 1 every 5ms

### Video Player (`src/drivers/video_player.cpp`)
- MJPEG video playback with JPEGDEC decoding
- Frame buffer pool (3×96KB JPEG buffers) staged in PIO PSRAM at offset 0x8000
- SD → PIO PSRAM → QMI buffer → JPEGDEC → framebuffer
- Exposed as `picocalc.video` in Lua

### PIO PSRAM (`src/drivers/pio_psram.c`)
- Second 8MB PSRAM on the PicoCalc v2.0 mainboard, accessed via PIO1 SPI
- Completely independent bus from QMI PSRAM/Flash XIP cache
- Used for: MP3 PCM ring buffer (addr 0x0000, 32KB) and video buffer pool (addr 0x8000)
- `pio_psram_init()` called early in `main()`; non-fatal if chip absent

### System Menu (`src/os/system_menu.c`)
- Triggered by the Sym key; detected via `kbd_consume_menu_press()` in the Lua debug hook
- Overlays the current framebuffer (darkened with `display_darken()`)
- Apps and OS can register items with `system_menu_add_item()` / `picocalc.sys.addMenuItem()`

### Config (`src/os/config.c`)
- Flat JSON key/value store persisted at `/system/config.json`
- `config_load()` at boot; `config_save()` writes back to SD
- Exposed to Lua as `picocalc.config.get(key)`, `.set(key, value)`, `.save()`, `.load()`
- Well-known keys: `"wifi_ssid"`, `"wifi_pass"`, `"brightness"`

### UI Widgets (`src/os/ui.c`, `text_input.c`)
- `ui_draw_header()` — titlebar with battery/WiFi/clock
- `ui_draw_footer()` — footer with status text
- `ui_draw_tabs()` — tab container with keyboard shortcuts
- `ui_text_input()` / `text_input_show()` — blocking modal text input
- `ui_confirm()` — yes/no confirmation dialog
- Exposed as `picocalc.ui` in the C API

### Hardware Pin Definitions
**All pins must be defined in `src/hardware.h` only — never hardcoded elsewhere.**

| Peripheral | Interface | Key Pins |
|---|---|---|
| ST7365P LCD | PIO0 SPI (100 MHz) | MOSI=GP11, SCK=GP10, CS=GP13, DC=GP14, RST=GP15 |
| SD Card | SPI0 (25 MHz) | MOSI=GP19, SCK=GP18, MISO=GP16, CS=GP17 |
| Keyboard (STM32) | I2C1 (10 kHz) | SDA=GP6, SCL=GP7, addr=0x1F |
| Audio L/R | PWM | GP26, GP27 |
| PIO PSRAM (mainboard) | PIO1 SPI | CS=GP20, SCK=GP21, MOSI=GP2, MISO=GP3 |
| USB VBUS Sense | GPIO | GP24 |

### Memory Map
- **SRAM heap**: ~28.8KB free after BSS (for `malloc`/`free` — tiny, must be freed promptly)
- **QMI PSRAM (8MB)**: Lua heap via `umm_malloc` at 0x11200000 (cached alias). ELF app data/BSS. Never mix `umm_malloc`/`umm_free` with standard `malloc`/`free`.
- **PIO PSRAM (8MB)**: MP3 PCM ring buffer (32KB). Accessed via `pio_psram_read`/`pio_psram_write`. Also exposed to native apps via `g_api.psram`.
- **Main stack**: 4KB in SCRATCH memory (`__StackBottom`=0x20081000, `__StackTop`=0x20082000)
- **Native app stack**: 8KB static SRAM buffer (`s_native_stack`), runs on PSP

## Coding Conventions

- Functions: `snake_case`
- Types: `_t` suffix (e.g., `picocalc_display_t`)
- Static module vars: `s_` prefix (e.g., `s_framebuffers`)
- Globals: `g_` prefix (e.g., `g_api`)
- Macros: `UPPER_CASE`
- All `malloc()` calls must be paired with explicit `free()`; `umm_malloc()` with `umm_free()`
- PSRAM allocations via `umm_malloc` (Lua heap) or PIO PSRAM APIs (audio buffers)
- BTN_* constants live in `os.h` (not keyboard.h)

## Extending the OS

### Adding a new hardware driver
1. Create `src/drivers/<name>.h` and `.c`
2. Add pin defines to `hardware.h`
3. Add a `picocalc_<name>_t` struct to `os.h`
4. Wire function pointers in `main.c`
5. Add sources and link libraries in `CMakeLists.txt`

### Exposing a new Lua API
1. Create `src/os/lua_bridge_<module>.c`
2. Write `static int l_<module>_<fn>(lua_State *L)` functions
3. Add to a `luaL_Reg` table
4. Call `register_subtable(L, "name", table)` in `lua_bridge_register()` (in `lua_bridge.c`)
5. Add the new `.c` file to `CMakeLists.txt`

## App Model

Apps are directories at `/apps/<name>/` on a FAT32 SD card containing:
- `app.json` — `{"id":"com.example.app", "name":"...", "description":"...", "version":"...", "author":"...", "requirements":[...]}`
- `main.lua` — Lua app (loop until `return` to exit to launcher)
- `main.elf` — native C/TinyGo app (ELF32 PIE; native takes priority if both present)

Pre-set globals (Lua): `APP_DIR` (e.g. `"/apps/hello"`), `APP_NAME`, `APP_ID`, `APP_REQUIREMENTS`.

Native apps receive `PicoCalcAPI *api` as their entry point argument. Use `api->sys->poll()` each frame and check `api->sys->shouldExit()` to handle the system menu exit.

### App Requirements

Apps can request elevated requirements via the `requirements` array in `app.json`:

```json
{
  "requirements": [
    "filesystem",         // default: read /apps/<dirname> and /data/<app_id>
    "root-filesystem",    // grants full SD card read/write access
    "http",               // app needs WiFi/network connectivity
    "audio",              // app needs audio output
    "clipboard"           // reserved for future use
  ]
}
```

The `APP_REQUIREMENTS` global in Lua is a table with boolean fields:
```lua
if APP_REQUIREMENTS.root_filesystem then
  -- App has full filesystem access
end
```

When `"root-filesystem"` is granted, the app bypasses the default sandbox (`/apps/<name>` read + `/data/<app_id>` write) and can access the entire SD card.

When `"http"` is granted, WiFi will remain connected after initial time sync (for power saving) so the app can make HTTP requests.

SD card auto-creates `/data/` and `/system/` on first mount.

## Lua API Reference

### picocalc.network

```lua
-- Connection status
picocalc.network.getStatus()          -- kStatusNotConnected | kStatusConnected | kStatusNotAvailable
picocalc.network.setEnabled(flag, fn) -- enable/disable WiFi; fn() called when done

-- HTTP connections (supports HTTPS via usessl=true)
local conn = picocalc.network.http.new(server, [port], [usessl], [reason])
conn:setKeepAlive(bool)
conn:setByteRange(from, to)
conn:setConnectTimeout(seconds)
conn:setReadTimeout(seconds)
conn:setReadBufferSize(bytes)
conn:get(path, [headers])
conn:post(path, [headers], data)
conn:query(path, [headers], data)     -- alias for post
conn:close()
conn:getError()                       -- nil or error string
conn:getProgress()                    -- bytesReceived, totalBytes
conn:getBytesAvailable()
conn:read([length])                   -- returns string or nil
conn:getResponseStatus()              -- HTTP status code integer
conn:getResponseHeaders()             -- table of {key=value}
conn:setRequestCallback(fn)           -- data arrived
conn:setHeadersReadCallback(fn)       -- headers parsed
conn:setRequestCompleteCallback(fn)   -- body complete
conn:setConnectionClosedCallback(fn)  -- connection closed or failed
```

Status constants: `picocalc.network.kStatusNotConnected` (0), `kStatusConnected` (1), `kStatusNotAvailable` (2).

## Not Yet Implemented

- `picocalc.display.drawBitmap` / raw bitmap blitting (use `picocalc.graphics.image` instead for image loading)
