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

To stage a multi-file app on hardware or the simulator, prefer the `push_app` MCP tool (`tools/picos_mcp.py`): it ships the whole directory as one ZIP and extracts it on-device via the `unzip <zip> <dest>` dev command (`rm <path>` cleans up), far faster than per-file transfers for asset-heavy apps. Dev commands run on an app stack (a 32 KB PSRAM stack at the launcher, inline on the app's stack inside an app), so `unzip` is safe at the launcher; `stack` reports the last launcher command's peak (`os_cmd_peak`, ~5.3 KB for a 66-file unzip). `exit` with no app running replies `Error: exit: no app running` and is otherwise ignored (it used to return from `launcher_run` and HardFault). In the simulator the `dev_command` RPC (`{"cmd": "unzip ..."}`) runs `ping`/`exit`/`unzip`/`rm` through the same handlers (`src/dev_ops.c`).

Tests: the simulator E2E suite (`SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto`; see `tests/e2e/README.md`, config in the repo-level `pytest.ini`), `make test-unit` (host C unit tests: `tests/unit/CMakeLists.txt` → `build_unit/`, ctest, ASan+UBSan; covers the pure modules `elf_plan`, `fs_path`, `app_manifest`, `config`/`appconfig` over an in-memory SD fake, `wav`, `audio_ring`, `zip_name`, `text_wrap`, `lua_numfmt` vs glibc, fonts), `python3 -m pytest tests/unit` and `make test-lua`. `make fuzz` runs the libFuzzer targets in `tests/fuzz/` (clang; `FUZZ_TARGETS=`, `FUZZ_SECONDS=`); CI runs them nightly (`.github/workflows/fuzz.yml`) and the unit job on every push (`unit.yml`). There is no linter. Lua fixture apps report through the test kit `tests/e2e/lib/picotest.lua` (staged as `/system/lib/picotest.lua`).

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
- **Core 1**: Runs the network stack (Mongoose/CYW43) and audio decode (MP3, fileplayer). Ticks every 1 ms (alarm on the Core 1 audio pool; audio pollers need that cadence), with the CYW43/Mongoose poll spaced to 5 ms while associated with no sockets open. Core 0 communicates via IPC ring buffer (`wifi_req_push()`). **Core 0 must never call `mg_*` functions directly.**

### Central API (`src/os/os.h`)
`PicoCalcAPI g_api` is a function pointer table wired in `main.c`. Sub-tables (all available to both Lua and native C apps unless noted):
- `picocalc.display` / `g_api.display` — drawing primitives, DMA flush, brightness, `drawImageNN`, `flushRows`
- `picocalc.input` / `g_api.input` — button state, character input, edge detection
- `picocalc.fs` / `g_api.fs` — file open/read/write/close/exists/size/listDir (Lua file handles: see "File handles" under App Model)
- `picocalc.sys` / `g_api.sys` — time, battery, log, reboot, system menu, poll (native apps), shouldExit
- `picocalc.wifi` / `g_api.wifi` — connect/disconnect/status/IP/SSID/isAvailable
- `picocalc.sysconfig` / `g_api.config` — system-wide key/value config (get/set/save/load); Lua only with the `"sysconfig"` requirement
- `picocalc.audio` / `g_api.audio` — tone generation, PCM streaming (playTone/stopTone/setVolume/startStream/stopStream/pushSamples)
- `picocalc.tcp` / `g_api.tcp` — raw TCP/TLS sockets (connect/write/read/close/available/getError/getEvents)
- `picocalc.ui` / `g_api.ui` — modal dialogs (textInput/textInputSimple/confirm)
- `g_api.http` — HTTP/HTTPS client (Phase 1; Lua exposes as `picocalc.network.http` OO objects)
- `g_api.soundplayer` — sample/fileplayer/MP3 player (Phase 1; Lua exposes as `picocalc.sound`)
- `g_api.appconfig` — per-app key/value config (Phase 1; Lua exposes as BOTH `picocalc.config` and `picocalc.appconfig` — same store, two names)
- `g_api.crypto` — crypto primitives: SHA-256/SHA-1/HMAC/AES-CTR/ECDH (Phase 1; Lua exposes as `picocalc.crypto`)
- `g_api.graphics` — image loading and drawing (Phase 2; Lua exposes as `picocalc.graphics.image`)
- `g_api.video` — MJPEG video playback (Phase 2; Lua exposes as `picocalc.video`)
- `g_api.modplayer` — MOD tracker music (Phase 2; Lua exposes as `picocalc.modplayer`)
- `g_api.zip` — ZIP extraction plus read-in-place archive handles (Lua exposes as `picocalc.zip`)
- `g_api.version` — 1 = Phase 1, 2 = Phase 2, 3 = `fs->browse`, 4 = clip rect + mode-7 plane, 5 = zip read-in-place handles, 6 = fonts (setFont/getFont/getFontWidth/getFontHeight/textWidth/loadFont/unloadFont/drawTextTransparent), 7 = video time seek/position, progress OSD, `hasEnded`, 8 = TLS verification: `http->setInsecure`, `tcp->connectEx` (`PCTCP_TLS`/`PCTCP_TLS_INSECURE`)

> ⚠️ **Config naming**: in Lua, `picocalc.config` (alias `picocalc.appconfig`) is the **per-app** store (`/data/<APP_ID>/config.json`); `picocalc.sysconfig` is the **system-wide** store (`/system/config.json`). Older docs had these inverted.

### App Lifecycle (`src/os/launcher.c`)
1. Scans `/apps/` on SD for dirs containing `main.lua` or `main.elf`
2. Reads `app.json` with a hand-rolled minimal JSON parser (no cJSON — flash savings)
3. Auto-detects app type: `main.elf` → native, `main.lua` → Lua (native wins if both present)
4. Shows scrollable menu with battery % header
5. Dispatches via `AppRunner` vtable (`src/os/app_runner.h`):
   - **Lua apps** (`src/os/lua_runner.c`): allocates a 64 KB VM stack, reads `main.lua`, then runs the whole VM lifetime on that stack (PSP): fresh `lua_State`, `lua_bridge_register()`, `lua_pcall()`, error screen, `lua_close()`
   - **Native apps** (`src/os/native_loader.c`): ELF32 PIE loader, relocates to PSRAM (code in SRAM if fits), runs on PSP (Process Stack Pointer). All header/segment/dynamic/relocation validation is in `src/os/elf_plan.c` (pure; shared with the simulator's `unicorn_runner.c`, host-tested in `tests/unit/test_elf_plan.c`, fuzzed by `tests/fuzz/fuzz_elf_plan.c`). Only `R_ARM_RELATIVE` is applied; `R_ARM_ABS32/GLOB_DAT/JUMP_SLOT` (undefined weak symbols in newlib's unwinder) are bounds-checked and left as-is; any other relocation type rejects the app
   - Both runners first install the app's identity (`src/os/app_identity.c`: id, `/apps/<dir>`, `/data/<id>`, requirement flags and the full requirement list, held in PSRAM) and clear it after teardown; for Lua this happens before `lua_bridge_register()`. Every enforcement check reads it through `app_identity_current()` / `app_identity_has_requirement(name)`, never the `APP_*` Lua globals
   - Both use `app_stack_run()` (`src/os/app_stack.c`): paints the stack, arms `PSPLIM` above a 32-byte guard (overflow = STKOF HardFault, crash record names `PSP (Lua VM)` / `PSP (native app)`), switches Thread mode to the PSP, calls the runtime, switches back. Only one runner is active, so there is one PSP user at a time; IRQs always use the MSP. The launcher itself stays on the MSP; its dev commands and screenshot save use `app_stack_run_os()` (32 KB PSRAM, owner `PSP (OS command)`), which runs inline instead when an app stack is already active, so `app_stack_run` never nests

### Lua Bridge (split across `src/os/lua_bridge_*.c`)
The Lua bridge is split into ~20 module files, coordinated by `lua_bridge.c`:
- `lua_bridge_appconfig.c` — per-app config (registered as both `picocalc.config` and `picocalc.appconfig`)
- `lua_bridge_audio.c` — tone/PCM streaming
- `lua_bridge_config.c` — system config (`picocalc.sysconfig`)
- `lua_bridge_display.c` — drawing primitives
- `lua_bridge_fs.c` — filesystem operations
- `lua_bridge_game.c` + `lua_bridge_game_camera.c` + `lua_bridge_game_save.c` + `lua_bridge_game_scene.c` — `picocalc.game` (camera, scene manager, save files)
- `lua_bridge_graphics.c` — image loading, sprites, spritesheets, tilemap, animations
  - **Object lifetimes:** every C pointer from one graphics object to another is anchored in a user value (`anchor_set`): a sprite keeps its image, stencil and tilemap, a spritesheet its image, a tilemap its tileset, an animation loop its frames. The display list (`s_sprites[]`) is mirrored by a registry table, so a sprite that has been `add()`ed is never collected while displayed; `remove()`/`removeSprites()`/`removeAll()` release it. `add()` is idempotent. Enumeration (`getAllSprites`, `performOnAllSprites`, `query*`, `overlappingSprites`, `moveWithCollisions`) and `getImage()` return the real objects. Blinkers are held weakly by `updateAll`. Sprite `width`/`height` are bounds only; drawing and `alphaCollision` use the image's (or source rect's) own size.
  - Only documented types are accepted: `image.loadFromBuffer` takes a string or a `qmibuf` handle (length checked against its size); `sprite.new` takes an image or nothing. Check userdata with `luaL_checkudata`/`luaL_testudata`, never `lua_touserdata`.
- `lua_bridge_input.c` — buttons, keyboard, key repeat
- `lua_bridge_json.c` — `picocalc.json` encode/decode
- `lua_bridge_mod.c` — `picocalc.modplayer` MOD music
- `lua_bridge_network.c` — WiFi control, HTTP client (OO connections with `HTTP_MT` metatable)
- `lua_bridge_perf.c` — performance profiling
- `lua_bridge_repl.c` — interactive Lua REPL
- `lua_bridge_sound.c` — sound samples, file player, MP3 player
- `lua_bridge_sys.c` — system functions, menu items
- `lua_bridge_terminal.c` — in-app virtual terminal widget
- `lua_bridge_ui.c` — modal dialogs
- `lua_bridge_video.c` — MJPEG video playback
- `lua_bridge_zip.c` — `picocalc.zip` archive extraction
- `lua_bridge_3d.c` — global `draw3DWireframeEx` software 3D helper

All `picocalc.*` Lua functions are `static int l_<module>_<fn>(lua_State *L)` wrappers. Registered via `luaL_Reg` tables passed to `register_subtable()`. Integer constants (button codes, color names) are pushed with `lua_pushinteger` / `lua_setfield`.

Every userdata/object type registers with `lb_register_type(L, mtname, methods, meta)` (`lua_bridge.c`): methods live in a separate `__index` table, the metatable holds only metamethods (`__gc`, `__close`, custom `__index`/`__newindex` functions, which get the methods table as upvalue 1) and is locked with `__metatable = false`, so `obj:__gc()` is "attempt to call a nil value". A finaliser can still hand an object to a method after the object's own `__gc` ran (resurrection), so each `__gc` leaves its object dead (pointer NULLed or a `destroyed` flag). For graphics, terminal, video, modplayer and the sound types, `check_*` then raises a Lua error. The handle types (fs file, zip archive, tcp, http, crypto) treat a finalised handle as closed: `close` and some getters accept it, and I/O raises. Image arguments go through `lb_check_image`, and draw paths that keep an image pointer (sprite `setSourceRect`, spritesheet `drawFrame`, animation loop `draw`) refuse freed pixels. `picocalc.modplayer.create()` returns the one live handle for the single MOD player.

Lua 5.4.7 is embedded with restricted stdlib: `base`, `table`, `string`, `math`, `coroutine`, `utf8`. Blocked: `io`, `os`, `package`, `debug`; base `dofile`/`loadfile` are removed. `load` is text-only (mode forced to `"t"`; bytecode is rejected), as are app `main.lua` and `sys.loadlib`.

Compile-time config lives in one place, `cmake/picos_lua.cmake` (`PICOS_LUA_DEFINITIONS`, applied `PUBLIC` by both the firmware and simulator builds): `LUA_32BITS=1`, `LUA_USE_LONGJMP=1`, `LUAI_MAXSTACK=1000`, `LUAI_MAXCCALLS=60`, `LUA_IDSIZE=60`. Upstream `luaconf.h` hard-codes these (`LUAI_MAXCCALLS` is already guarded in `llimits.h`), so the same file also patches it (`#if !defined` guards, idempotent, marker comment on line 1) — at CMake configure time, from `make download-lua`, and in the CI workflows. `_Static_assert`s in `lua_bridge.c` fail the build if the patch is lost.
- Numbers: `lua_Integer` is 32-bit (wraps at ±2^31; hex literals like `0xDEADBEEF` wrap to negative, `%x` still prints the 32-bit pattern), `lua_Number` is single-precision `float` (~7 significant digits, integers exact only to 2^24, `1e39 == math.huge`). `sys.getTimeMs()` goes negative after ~24.8 days uptime (differences still wrap correctly); `getClock().epoch` overflows in 2038; file sizes above 2 GB read negative.
- Float formatting (`tostring`, `..`, `string.format` `%a/%e/%f/%g`, JSON, REPL) goes through `src/os/lua_numfmt.c` on both targets, never the C library (`luaconf.h`'s `l_sprintf` is patched to `picos_lua_sprintf`): output matches glibc exactly (`tostring(51.0)` is `"51.0"` on device too, not pico_printf's `"51.00000"`), except NaN always prints `nan`. Subnormals (<1.2e-38) print `0.0` on the device because the RP2350 flushes them when a float is promoted to double.
- Bridge quantity arguments (coordinates, sizes, durations, volumes, rates, frame indices; `lb_checkint`/`lb_optint` in `lua_bridge.c`) round floats to nearest (ties toward +inf), so `199.99998` draws at 200; NaN/inf and floats beyond ±2^24 are argument errors. This includes the `display` primitives that used to truncate (`fillRect`, `drawLine`, `drawText`, …), so `fillRect(10.5, …)` and `img:draw(10.5, …)` both land on x = 11. Narrow sinks clamp instead of wrapping (`lb_clamp_int`): volumes, `rgb` components, effect factors and brightness to their documented range, unsigned times/positions at 0. Identifiers (handles, enums, colours, masks, byte counts, ports) still require exact integers.
- `LUAI_MAXSTACK` counts stack *slots*, not frames: a typical frame costs 10–13 slots, so recursion tops out around 80–100 levels. 500 (the old nominal value) overflowed the minesweeper flood fill on ~3% of first clicks; 1000 is the smallest value that runs every shipped app.
- `LUAI_MAXCCALLS=60` caps nested C calls and parser levels. Lua→C→Lua recursion (e.g. nested `string.gsub` callbacks, ~864 bytes of C stack per level on device) stops at ~58 levels with a catchable `"C stack overflow"` error, before the 64 KB VM stack runs out (measured peak 51.4 KB). Raise it only together with `LUA_VM_STACK_SIZE` in `lua_runner.c`.

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

### TLS policy (`src/drivers/wifi.c`, `ca_bundle.c`, `rng.c`)
- **Verification on by default.** Every HTTPS and `tls://` TCP connection verifies the server certificate (chain to a root in `src/drivers/ca_bundle.c`, validity dates, host name via SNI). Failures reach Lua as `"TLS: certificate not trusted …"` / `"… expired"` / `"… does not match the host name"`.
- **SNTP gate.** Validity dates need the wall clock, so a verifying connection is refused until SNTP has set it (`clock_is_set()`), with an error starting `"clock not set"` (`WIFI_TLS_ERR_CLOCK`); if SNTP had given up, the refusal restarts it, so a retry a few seconds later works.
- **Opt-out, per connection:** Lua `conn:setInsecure(true)` (HTTP, before `get`/`post`) or `tcp:setInsecure(true)` (before `connect`); native `http->setInsecure(c, true)` or `tcp->connectEx(host, port, PCTCP_TLS | PCTCP_TLS_INSECURE)`. Skips verification and the clock gate; logs `[TLS] <host>: certificate verification DISABLED`. For self-signed dev servers only.
- **TCP TLS sockets** report connected (`TCP_CB_CONNECT`, writable) only after the handshake (`MG_EV_TLS_HS`); the connect timeout covers the handshake.
- **CA bundle** — 11 roots (ISRG X1/X2, GTS R1/R4, Sectigo E46/R46, USERTrust ECC/RSA, DigiCert G2, SSL.com TLS ECC/RSA 2022), chosen from the chains of `picos.jeffory.dev`, `github.com` and `*.githubusercontent.com`. It is parsed once at boot (`wifi_tls_init`, on the OS stack) into one chain every verifying connection shares; `tests/unit/test_ca_bundle.c` checks it parses whole and that captured picos.jeffory.dev / github.com / release-assets chains verify against it. To add a root: put its PEM in `tools/ca/`, add it to `ROOTS` in `tools/gen_ca_bundle.py` (with why), run `python3 tools/gen_ca_bundle.py` (`--check` verifies the committed files, `--extract <system bundle>` refreshes the PEMs). Check a host's chain with `openssl s_client -connect HOST:443 -servername HOST -showcerts`.
- **Randomness.** `src/drivers/rng.c`: RP2350 TRNG (von Neumann + CRNGT + autocorrelation health tests; software-reset after every use so pico_rand never meets a halted block) → mbedTLS entropy pool → one CTR_DRBG per core, seeded only at shallow points (Core 0 on the OS stack before `wifi_init`, Core 1 at the top of `core1_entry`); Core 1 draws come from a 512-byte pool that `wifi_poll` refills. Mongoose's `mg_random` (`MG_ENABLE_CUSTOM_RANDOM`) and `picocalc.crypto` use it; `MBEDTLS_ENTROPY_HARDWARE_ALT` (get_rand_64) is off. It fails closed: no seeded/working DRBG → `mg_random` returns false with zeros (no get_rand fallback), TLS is refused and `crypto.randomBytes` raises. The `stack` dev command reports `core1_peak` (Core 1 stack painted at boot) alongside `msp_peak`.

### HTTP Client (`src/drivers/http.c`)
- Mongoose-based HTTP/1.1 client running on Core 1
- Static pool of 8 simultaneous connections (`HTTP_MAX_CONNECTIONS`)
- **HTTPS supported** via `pico_lwip_mbedtls` + `pico_mbedtls` (see `src/mbedtls_config.h`), certificate-verified (see TLS policy)
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
  - 8 sample slots + 8 player slots, mixed by `sound_mixer_process()` inside audio.c's DMA refill ISR on Core 1. Anything that frees a sample or changes which sample a player reads takes the mixer lock (`s_mix_cs`, a striped critical section); `sound_sample_destroy()` stops and detaches every player still using the sample first. A player never owns its sample.
  - Lua: a `sampleplayer` keeps its sample alive as a user value (`sampleplayer(path)` and `sample:play()` anchor a sample of their own; `getSample()` returns that object). Collected players and samples give their slots back (and a player its callback slots); freed handles hold NULL and are rejected.
- `fileplayer.c` — streaming WAV playback from SD card. Each fileplayer owns its file (and data-chunk fields); one plays at a time on the stream. Flow control: with F stream-ring frames free, a player at speed r reads at most floor(F·r) frames, so nothing is dropped and a WAV plays for its real length.
- `mod_player.c` — MOD music (pocketmod), 128 frames per update, only when the ring has room.
- `mp3_player.c` — MP3 decoding via libmad; PCM ring buffer (32KB) in PIO PSRAM; its own DMA ISR (DMA_IRQ_1) reads from an 8KB SRAM staging buffer, refilled by Core 1 with that ISR masked around the index updates. The decode loop continues only when a refill brought new data (SD busy / fed ring empty → next tick; EOF → loop back at most once per update, or finish). Stop/pause ramp to silence and wait for the ISR to play the ramp before the DMA stops.
- `mp3_player_update()`, `fileplayer_update()` and `mod_player_update()` called on Core 1 every 1 ms tick
- **Locks (Core 0 ↔ Core 1)**: each player has a mutex (`fileplayer.c` `s_lock`, `s_mod_mutex`, `s_mp3_mutex`) guarding its file / data / state. Core 0 takes it blocking, only for field updates (files are opened, closed and freed outside it, after detaching); Core 1's update only ever **try-locks** it and skips the tick. Core 1 never blocks on the SD card either: it reads with `sdcard_try_fread_at()` (seek + read under one try-lock, `SDCARD_BUSY` when Core 0 holds the card) and keeps its own file offset — never `sdcard_fseek` on Core 1.
- Volume (master, MP3) scales the signed sample, i.e. about PWM mid-scale; silence is always `PWM_MID`.
- Volumes are 0-100 everywhere (`audio.setVolume`, sampleplayer, fileplayer, mp3player, modplayer); larger values clamp to 100 (the mixers scale by vol/100, so more would overdrive).

### Video Player (`src/drivers/video_player.cpp`)
- MJPEG video playback with JPEGDEC decoding (JPEGDEC state lives in static SRAM)
- Frame buffer pool (3×96KB JPEG buffers) in QMI PSRAM; Core 1 prefetches frame N+1 from SD into the pool while Core 0 decodes frame N
- Frame + audio chunk indices sized from the AVI header (cap `VIDEO_MAX_FRAME_INDEX`); beyond the cap playback/seek fall back to sequential chunk scanning
- Seeks clamp and never wrap; with loop off the last frame is held (`ended`). Built-in progress OSD drawn into the frame after decode
- Exposed as `picocalc.video` in Lua; hardware-only (the simulator stubs it)

### PIO PSRAM (`src/drivers/pio_psram.c`)
- Second 8MB PSRAM on the PicoCalc v2.0 mainboard, accessed via PIO1 SPI
- Completely independent bus from QMI PSRAM/Flash XIP cache
- Layout (`pio_psram.h`): MP3 PCM ring `0x0000` (32KB), video region `0x8000` (256KB, reserved), apps from `PIO_PSRAM_APP_BASE` = `0x48000` (288KB)
- Lua `sys.pioPsramRead/Write` may only touch `[PIO_PSRAM_APP_BASE, chip end)`: anything overlapping the OS region, negative, or past the chip raises a Lua error (`pio_psram_app_range_check`, 64-bit, host-tested). Native `g_api.psram` is unchecked.
- Lua `sys.qmiPsramAlloc(size)` returns a bounds-checked buffer handle (userdata owning a `umm_malloc` block, freed by `qmiPsramFree` or GC); `qmiPsramRead/Write(handle, offset, …)` raise on out-of-range or freed handles. No raw pointers reach Lua.
- DMA vs the XIP cache (`pio_psram_xip.h`): a source in cached QMI PSRAM (`0x11…`) has its lines cleaned (`xip_cache_clean_range`) and is read through the uncached alias; a cached destination gets whole lines DMA'd with `xip_cache_invalidate_range` before and after, and its partial end lines copied by the CPU from a bounce buffer (never invalidate a line shared with other data). Transfers hold the driver lock one 4KB segment at a time and yield it to a waiting core between segments.
- `pio_psram_init()` called early in `main()`; non-fatal if chip absent

### System Menu (`src/os/system_menu.c`)
- Triggered by the Sym key; detected via `kbd_consume_menu_press()` in the Lua debug hook
- Overlays the current framebuffer (darkened with `display_darken()`)
- Apps and OS can register items with `system_menu_add_item()` / `picocalc.sys.addMenuItem()`

### Config (`src/os/config.c`)
- Flat JSON key/value store persisted at `/system/config.json`
- `config_load()` at boot; `config_save()` writes back to SD
- Exposed to Lua as `picocalc.sysconfig.get(key)`, `.set(key, value)`, `.save()`, `.load()` (NOT `picocalc.config` — that's the per-app store, see the naming warning above), registered only for apps whose `app.json` declares `"sysconfig"` (otherwise `picocalc.sysconfig` is nil). `wifi_pass` is write-only: `get("wifi_pass")` always returns nil, `set` works. Native apps' `g_api.config` is unrestricted.
- Well-known keys: `"wifi_ssid"`, `"wifi_pass"`, `"brightness"`, `"dim_timeout_s"` (idle screen-dim timeout in seconds; `"0"` disables; default 60)

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
- **SRAM heap**: ~2.6KB (`__end__`=0x2007f580 to `__HeapLimit`=0x20080000; the double framebuffer takes 400KB of BSS) — effectively none; scratch buffers go through `umm_malloc`
- **QMI PSRAM (8MB)**: Lua heap via `umm_malloc` at 0x11200000 (cached alias). ELF app data/BSS. Never mix `umm_malloc`/`umm_free` with standard `malloc`/`free`.
- **PIO PSRAM (8MB)**: MP3 PCM ring buffer (32KB). Accessed via `pio_psram_read`/`pio_psram_write`. Also exposed to native apps via `g_api.psram`.
- **Main stack (MSP)**: 4KB in SCRATCH memory (`__StackBottom`=0x20081000, `__StackTop`=0x20082000), `MSPLIM`-guarded. Boot, the launcher (menus, USB MSC) and every IRQ run here; dev commands and screenshot save go through `app_stack_run_os()` onto a short-lived 32KB PSRAM PSP stack (crash record `PSP (OS command)`); measured peak ~2.2-2.5KB. The `stack` dev command prints this peak and the running app's stack peak
- **Lua VM stack**: 64KB `umm_malloc` (QMI PSRAM) per launch, runs on PSP. PSRAM because no SRAM region that size exists; the cost is ~10-50% on C-call-heavy Lua code (pure Lua loops unchanged)
- **Native app stack**: 16KB SRAM `malloc` if available (in practice never), else 64KB `umm_malloc`; runs on PSP

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

### Changing the native API (`PicoCalcAPI`)
The struct exists three times: `src/os/os.h`, `sdk/native/os.h` and the simulator's trampoline tables (`simulator/unicorn_trampolines.c`). Never insert a field before `version` (it would move); add new fields after it, in the same order in both headers and the trampolines, bump `g_api.version` in `src/main.c` and the trampolines' version write together, then run `python3 tools/check_native_abi.py --cc arm-none-eabi-gcc` (CI's `native-sdk` job) and extend and rebuild the E2E probe (`tests/e2e/native/api_probe.c`, `make -C tests/e2e/native`); `tests/e2e/test_native.py` checks the running layout.

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

Pre-set globals (Lua): `APP_DIR` (e.g. `"/apps/hello"`), `APP_NAME`, `APP_ID`, `APP_REQUIREMENTS`. They are convenience copies: the sandbox and the per-app stores read the C-side identity (`app_identity`), so an app that rewrites them gains nothing.

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
    "clipboard",          // reserved for future use
    "sysconfig",          // picocalc.sysconfig (system config; wifi_pass write-only)
    "system-update"       // sys.applyUpdate — OS updater/store only (see below)
  ]
}
```

The `APP_REQUIREMENTS` global in Lua is a table with boolean fields:
```lua
if APP_REQUIREMENTS.root_filesystem then
  -- App has full filesystem access
end
```

Without `"root-filesystem"`, the sandbox (`fs_sandbox_check` → `fs_path_allowed` in `src/os/fs_path.c`, host-tested) allows reads of the app's own `/apps/<dir>` and of `/system/lib/`, and read + write of `/data/<app_id>`; relative paths and any `..` are refused. It guards `picocalc.fs.*` and the image, font and zip loaders, the sound loaders (`sound.sample`/`sampleplayer` paths, `sample:load`, `sample:save`, `fileplayer:load`, `mp3player:load`), `modplayer:load`, `video:load` and `crypto.sha256File`. When `"root-filesystem"` is granted, the app bypasses the sandbox and can access the entire SD card.

**File handles.** `picocalc.fs.open(path [, mode])` returns a full userdata (`lua_bridge_fs.c`, metatable `picocalc.fs.file`, hidden via `__metatable = false`) or `nil, err`. `fs.read(h, n)` and `h:read(n)` are the same function (likewise `write`/`close`/`seek`/`tell`; methods come from a separate `__index` table). A closed handle raises "attempt to use a closed file"; `close` is idempotent and `fs.close(nil)` is a no-op; any other value (a string, a light userdata, another module's userdata) is a type error. `read` rejects a negative length and clamps the length to the bytes left in the file; it and `readFile` read straight into a Lua buffer, so `readFile` of a file larger than free memory raises a memory error (it does not return `nil`). A dropped handle is closed by `__gc`, `local f <close> = fs.open(...)` closes at scope exit, and every open file is also on the running app's open-file list (`app_files_*` in `app_identity.h`), which `lua_run` sweeps after `lua_close` — FatFS allows only 16 open files (`FF_FS_LOCK`), so a leak would otherwise break `f_open` until a reboot.

`picocalc.config` always reads and writes the running app's own `/data/<app_id>/config.json`: the store is bound to the identity's id on each call and unbound at every app start and exit, so one app never sees another's keys.

`picocalc.sys.applyUpdate(path)` is registered only when the app declares `"system-update"` AND is an OS app: its id is `com.picos.updater` or `com.picos.store`, or its directory is under `/system/`. That allow-list is **self-declared** (an app writes its own `app.json`, so any app can claim an OS id); it narrows the API, it is not the boundary.

**OTA update policy — the real controls are the signature, the confirm and the boot token:**
- **Signature.** An image is flashed only if `/system/update.sig` (DER ECDSA P-256 over SHA-256 of the image, as `openssl dgst -sha256 -sign` writes) verifies against the public key embedded in the running firmware (`src/os/ota_verify.c`, shared verbatim with the simulator and `tests/unit/test_ota_verify.c`). `/system/update.sha256` (strict: exactly 64 hex digits + optional newline) is still required but is only a cheap pre-check. `applyUpdate` checks sandbox → size/vector table → checksum → signature before its dialog; the boot re-checks checksum + signature over the exact bytes it is about to write (PSRAM copy).
- **Confirm.** `applyUpdate` shows a `ui_confirm` naming the file; declining returns `false, "cancelled"`. `ui_confirm` discards input queued before it appears (STM32 FIFO, injected keys), ignores input for 400 ms, and needs a fresh Enter press edge (or `y`), so an app cannot pre-load a "yes".
- **Boot token.** The boot applies an update only with the one-shot OTA token (`OTA_MAGIC` in watchdog scratch[1], set only by C in `ota_trigger_update` — after the confirm, or by the `reboot-ota` dev command that `make flash-ota` sends; `reboot-ota` is dropped, not deferred, if it arrives while an app runs). A `/system/update.bin` found without the token is renamed to `update.bin.stale` (with its `.sha256`/`.sig`) and logged. An image the boot refuses (bad checksum/signature, too big, OOM…) is renamed to `update.bin.rejected` (+ `.sha256`/`.sig`), the reason is shown and logged as `OTA REJECTED` in `/system/error.log`, and it is never retried. The HardFault record also uses scratch[1] (stacked PC), so `main()` zeroes it on the crash path.
- **Keys.** The embedded key comes from the CMake option `PICOS_UPDATE_PUBKEY_PEM`, default `tests/keys/picos-update-TEST-public.pem` — a TEST key whose private half is in the repo, so dev builds accept anything signed with it and `make flash-ota` signs with it (`tools/ota_flash.py --key`, or `make flash-ota OTA_KEY=<priv.pem>`). `.github/workflows/release.yml` refuses to build without the `UPDATE_SIGNING_KEY` secret (a P-256 private key PEM): it embeds that key's public half (`-DPICOS_REQUIRE_RELEASE_UPDATE_KEY=ON` rejects the test key) and publishes `picocalc_os.sig` next to `.bin`/`.sha256`. `tools/sign_update.py sign|verify|genkey` wraps the openssl CLI; a new key pair is `tools/sign_update.py genkey priv.pem pub.pem`. Firmware built with one key only accepts images signed by that key — changing keys needs one UF2 flash (or an OTA signed with the OLD key).
- The store and updater fetch `picocalc_os.sha256` and `picocalc_os.sig` from the release; the store refuses to download an image when either is missing.

When `"http"` is granted, WiFi will remain connected after initial time sync (for power saving) so the app can make HTTP requests.

The `id` is a path component (`/data/<id>`), so it must be 1-79 characters of `[A-Za-z0-9._-]` with no `..` and no trailing `.` (FatFS strips it, so `com.victim.` would alias `/data/com.victim`); it is folded to lower case (FatFS is case-insensitive, so each app has one data directory), and the sandbox compares paths case-insensitively. The launcher refuses an app with an invalid id (on-screen reason + `/system/error.log` entry, the same path as the `min_psram_kb` refusal). A missing `id` defaults to `local.<dir>` with any other character (and a trailing `.`) replaced by `_`. The scan logs a warning when two apps' ids fold to the same value (they would share `/data/<id>`). `fs.browse(start)` checks `start` for read access and falls back to the app's data root.

SD card auto-creates `/data/` and `/system/` on first mount.

### Save slots (`picocalc.game.save`)
- Each slot is `/data/<app_id>/saves/<name>.json` (the directory is created on first use); the id comes from the C-owned app identity, never a Lua global. Serialised with the shared `picocalc.json` encoder/decoder (`lua_json_encode_push` / `lua_json_decode_push` in `lua_bridge_json.c`).
- Names must pass `fs_name_valid` (`src/os/fs_path.c`): 1–128 bytes of `[A-Za-z0-9._-]`, no `..`, no leading `.`. Anything else: `set` returns `false, "invalid save name"`, `get` nil, `exists`/`delete` false.
- Migration: saves used to live in a shared `/saves/<name>.json`, which game.save never modifies or removes. When an app **reads** (`get`/`exists`) a name it has no slot for and the legacy file exists, the file is **copied** into its slot; `set`/`delete` never copy. A marker `saves/.migrated-<name>` (written on the copy, or when `set`/`delete` touch a name that has a legacy file) stops a later `delete` from being undone by a fresh copy; markers start with `.` so `list()` never shows them. `list()` shows only the app's own slots.
- Encoding: whole floats are written with `.0` (`2.0` → `2.0`, not `2`) so `math.type` survives a round trip (`picocalc.json` too).

Optional `"min_psram_kb": N` in `app.json` makes the launcher refuse to start the app (with an on-screen reason and an error.log entry) unless the PSRAM heap has a single free block of at least N KB. Use it for apps that need one large contiguous allocation; total free bytes are not the test, the largest block is.

### Error and crash records (`src/os/crashlog.c`)
- `/system/error.log` — app-level failures written while the OS is alive: Lua runtime errors, Lua panics, native loader errors (ELF too big, out of PSRAM, stack overflow), launch refusals. Every entry ends with a `Heap:` line (free / largest block / fragmentation).
- `/system/crashlog.txt` — OS-level records written at boot: HardFault dumps decoded from watchdog scratch (now with the app name) and `UNCLEAN EXIT` entries when an app hung until the watchdog fired.
- `/system/running.txt` — dirty-exit marker written at launch and deleted when the runner returns. If it survives to the next boot the app never came back to the launcher; the boot code names it on screen for hardfault/watchdog cases. A plain power-off mid-app also leaves it, but that is normal use and is only echoed to serial. Intentional reboots (system menu, `sys.reboot`, dev `reboot`) clear it first.
- `picocalc.sys.getMemInfo()` returns `psram_largest_block` and `psram_fragmentation` alongside the totals.

## Lua API Reference

System Lua libraries live in `/system/lib/` and load via `picocalc.sys.loadlib(name)`: `system/lib/download.lua` provides `download.toFile(url, dest, opts)` (blocking HTTP(S)-to-SD streaming download), and `picocalc.crypto.sha256File(path)` returns a file's SHA-256 as lowercase hex for checksum verification.

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

## Simulator Notes

`make simulator` builds `build_sim/picos_simulator` (SDL2 + Unicorn Engine for native ELF apps). Known divergences from hardware:

- `picocalc.crypto` is **absent** (Lua and native) — mbedTLS is firmware-only (`simulator/CMakeLists.txt` excludes `lua_bridge_crypto.c`; native crypto trampolines are stubs except `randomBytes`).
- Display post-effects (`effectInvert`…`effectPosterize`) are no-ops on the native (Unicorn) path; Lua-side effects work.
- Hardware vertical scroll (`setScrollArea`/`setScrollOffset`/`getScrollOffset`) is emulated: flushes land in a GRAM analog and presents/screenshots are composed through the scroll registers, mirroring the ST7365P ring semantics for the visible 320 lines (the real chip's extra 160 frame-memory lines are not modelled).
- The launcher caches the app list at boot, but the `launch_app` RPC rescans `/apps` once when a name is not found, so newly staged apps launch without a restart; `rescan_apps` forces a rescan (e.g. after editing an existing app's `app.json`).
- Boot mirrors `src/main.c`: `config_load()`, idle-dim init, network, Core 1, `system_menu_init()`, launcher. Idle dim stays inert (the keyboard stub never polls it).
- The SD HAL models the open-file COUNT of FatFS's `FF_FS_LOCK = 16` only: a 17th concurrently open file is refused (`[SIM] too many open files`), so leaked handles fail in the simulator as they do on the device. FatFS's per-file sharing rules (a file already open for writing cannot be opened again) are not modelled.
- SD paths resolve `..` lexically and anything that would leave the SD root is refused (`[SIM] SD escape: <path>` on the `err` log source).
- Test control channel (`simulator/sim_socket_handler.c`, `sim_test_control.c`): `get_log_buffer {since_seq, tail}` returns `{lines:[{seq,t_ms,src,text}], next_seq, dropped, more}` with `src` = `lua`/`native`/`os`/`err`; `subscribe {"logs":true}` pushes `log {seq,src,text}` notifications; `app.exited` carries `{name, id, found, result: returned|error|exit_sentinel|load_failed, error, runtime_ms, launch_id}`; `launch_app` returns `{queued, busy, launch_id}` and `get_last_outcome` returns the last `app.exited` params (backfill when a notification was dropped); `display_stats.present_count`; `inject_*` return `input_seq` and `get_input_state` reports `consumed_seq`. `--test-mode` makes Lua error screens and launch refusals return at once (the text is on the `err` source). The TCP listener binds 127.0.0.1 only; `--unix-socket PATH|none` overrides or disables the `./picos_control` UNIX socket (the E2E harness passes `none`). Python client: `tests/e2e/picos_simulator.py`.
- TLS certificate verification, the SNTP clock gate and the TRNG are firmware-only: the simulator's libcurl transport keeps its own TLS policy and only stores the `setInsecure` flag. The OTA *checks* (checksum + signature against the TEST key, `src/os/ota_verify.c`) do run in the simulator; flashing does not.
- `picocalc.video` is stubbed (no decoder): `player()` returns nil-backed handles; every video trampoline is a no-op.
- Audio: the PCM stream is the firmware's ring (`audio_ring.h`), drained at 44.1 kHz by wall clock on the Core 1 thread (`sim_stream_drain()` stands in for the DMA ISR), so `audio_ring_free()` and flow control behave as on hardware; the fileplayer and MOD player are the firmware's `fileplayer.c`/`mod_player.c`. The sample mixer and MP3 player are simulator copies in `sim_audio.c` (keep their logic in step with `sound.c`/`mp3_player.c`); tone, samples and MP3 go to SDL directly.
- Networking: `make simulator` replaces `wifi.c`/`http.c`/`tcp.c` with its own libcurl/POSIX layer (`simulator/sim_wifi.c`, `sim_http.c`, `sim_tcp.c`: always online, none of the firmware's Core 0/Core 1 races). `make simulator-net` (`-DSIM_FIRMWARE_NET=ON`, `build_sim_net/`; also `simulator-net-asan`/`simulator-net-tsan`) compiles the firmware's own `wifi.c`, `http.c`, `tcp.c` on the vendored Mongoose as `MG_ARCH_UNIX` (BSD sockets, **no TLS**: HTTPS and `tls://` are refused), Core 0 = main thread, Core 1 = the sim's Core 1 thread. Host shims are in `simulator/net/` (spinlocks → pthread mutexes with the SDK's claimable ids 24-31; `get_core_num`; host randomness for `rng.h`/`mg_random`; `core1_calloc` → calloc; a stand-in Wi-Fi interface that joins `SimulatorWiFi` as 127.0.0.1 at boot; SNTP and the connectivity check stay on loopback). The firmware files only gain `PICOS_SIM_FIRMWARE_NET` guards (the TLS section and the `time()` override in `wifi.c`) and `#ifndef` defaults (`WIFI_SNTP_URL`, `WIFI_CHECK_URL`, `PSRAM_UNCACHED_OFFSET`); their firmware objects are unchanged. `--build-info` prints `firmware_net=1`; `tests/e2e/test_network_firmware.py` (local servers in `tests/e2e/net_servers.py`) runs only against that build.
- `umm_*` is a counting allocator over host malloc: `umm_free_heap_size()` / `get_heap_info` report 8 MB minus the live umm/Lua bytes, but the largest free block is approximated by the free total and fragmentation is always 0 (fragmentation and `min_psram_kb` refusals stay device-only).
- Sanitizer builds: `make simulator-asan` (ASan+UBSan, `build_sim_asan/`) and `make simulator-tsan` (`build_sim_tsan/`), built with clang. Run the E2E suite against one with `PICOS_SIM_BINARY=build_sim_asan/picos_simulator`; `asan_only` tests run only there. TSan is informational (the sim's RPC socket thread and Core 1 audio state race by design).
- Everything else (zip including read-in-place archive handles, modplayer, display clip rect, drawPlane, tilemap, sprites) mirrors firmware, including `g_api.version = 8`.

## Not Yet Implemented

- `picocalc.display.drawBitmap` / raw bitmap blitting (use `picocalc.graphics.image` instead for image loading)
- Native-app HTTP callbacks (`http_fire_c_pending` is a no-op; native apps must poll `http->isComplete()`)
