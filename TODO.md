# PicOS TODO

## Lua API gaps — C exists, not wired to Lua

These are fully implemented in C but missing from `lua_bridge.c`. All are quick wins.

### `picocalc.sys`

| Function | Status | Notes |
|---|---|---|
| `sys.reboot()` | **[done]** | Watchdog-based reboot added to `l_sys_lib` |
| `sys.isUSBPowered()` | **[done]** | Stub returning false added to `l_sys_lib`; real VBUS check via GP24 still TODO (see System section) |

### `picocalc.fs`

| Function | Status | Notes |
|---|---|---|
| `fs.size(path)` | **[done]** | `l_fs_size` wrapper added to `l_fs_lib` |
| `fs.listDir(path)` | **[done]** | Returns `{ {name, is_dir, size}, ... }` table via `listdir_cb` callback |
| `fs.appPath(name)` | **[done]** | Returns `/data/<dirname>/<name>`; auto-creates the data directory |

### `picocalc.display`

| Function | Status | Notes |
|---|---|---|
| `display.textWidth(str)` | **[done]** | `l_display_textWidth` wrapper added to `l_display_lib` |

### `picocalc.network`

| Function | Status | Notes |
|---|---|---|
| `http.new(..., usessl=true)` | **[done]** | HTTPS support enabled via patched `altcp_tls_mbedtls_compat.c` for MbedTLS 3.x compatibility. |

---

## `g_api` struct not fully wired in `main.c`

`g_api.fs` is never assigned in `main.c` (left NULL).
The Lua bridge calls C functions directly so Lua is unaffected, but the C app loader
(future) will need this populated.

- [ ] Wire `g_api.fs = &s_fs_impl` once a `picocalc_fs_t` struct is built
- [x] Wire `g_api.audio` — implemented in this commit
- [x] Wire `g_api.wifi` — was already implemented

---

## Audio — implemented

Driver location: `src/drivers/audio.c` (created)
Hardware: PWM on GP26 (left) and GP27 (right) — defined in `hardware.h`

- [x] Create `src/drivers/audio.h` / `audio.c`
- [x] `audio_init()` — configure PWM slices on GP26/GP27
- [x] `audio_play_tone(uint32_t freq_hz, uint32_t duration_ms)` — square wave; 0ms = indefinite
- [x] `audio_stop_tone()`
- [x] `audio_set_volume(uint8_t vol)` — 0–100, logarithmic scale
- [x] Add `picocalc.audio.playTone(freq, duration)`, `stopTone()`, `setVolume(vol)` to Lua bridge
- [x] Wire `g_api.audio` in `main.c`

---

## Sound - implemented

- [x] sound.sampleplayer.new(path)
- [x] sound.sampleplayer.new(sample)
- [x] sound.sampleplayer:copy() - partial
- [x] sound.sampleplayer:play([repeatCount], [rate])
- [x] sound.sampleplayer:playAt(when, [vol], [rightvol], [rate])
- [x] sound.sampleplayer:setVolume(left, [right])
- [x] sound.sampleplayer:getVolume()
- [ ] sound.sampleplayer:setLoopCallback(callback, [arg])
- [ ] sound.sampleplayer:setPlayRange(start, end)
- [ ] sound.sampleplayer:setPaused(flag)
- [x] sound.sampleplayer:isPlaying()
- [x] sound.sampleplayer:stop()
- [ ] sound.sampleplayer:setFinishCallback(func, [arg])
- [x] sound.sampleplayer:setSample(sample)
- [ ] sound.sampleplayer:getSample()
- [ ] sound.sampleplayer:getLength()
- [ ] sound.sampleplayer:setRate(rate)
- [ ] sound.sampleplayer:getRate()
- [ ] sound.sampleplayer:setRateMod(signal)
- [ ] sound.sampleplayer:setOffset(seconds)
- [ ] sound.sampleplayer:getOffset()
 - [x] sound.fileplayer.new([buffersize]) - For music
 - [x] sound.fileplayer.new(path, [buffersize])
 - [x] sound.fileplayer:load(path)
 - [x] sound.fileplayer:play([repeatCount])
 - [x] sound.fileplayer:stop()
 - [x] sound.fileplayer:pause()
 - [x] sound.fileplayer:isPlaying()
 - [x] sound.fileplayer:getLength()
 - [ ] sound.fileplayer:setFinishCallback(func, [arg])
 - [x] sound.fileplayer:didUnderrun()
 - [ ] sound.fileplayer:setStopOnUnderrun(flag)
 - [x] sound.fileplayer:setLoopRange(start, [end, [loopCallback, [arg]]])
 - [ ] sound.fileplayer:setLoopCallback(callback, [arg])
 - [ ] sound.fileplayer:setBufferSize(seconds)
 - [ ] sound.fileplayer:setRate(rate)
 - [ ] sound.fileplayer:getRate()
 - [ ] sound.fileplayer:setRateMod(signal)
 - [x] sound.fileplayer:setVolume(left, [right, [fadeSeconds, [fadeCallback, [arg]]]])
 - [x] sound.fileplayer:getVolume()
 - [x] sound.fileplayer:setOffset(seconds)
 - [x] sound.fileplayer:getOffset()
- [x] sound.sample.new(path)
- [ ] sound.sample.new(seconds, [format])
- [ ] sound.sample:getSubsample(startOffset, endOffset)
- [x] sound.sample:load(path)
- [ ] sound.sample:decompress()
- [x] sound.sample:getSampleRate()
- [ ] sound.sample:getFormat()
- [x] sound.sample:getLength()
- [ ] sound.sample:play([repeatCount], [rate])
- [ ] sound.sample:playAt(when, [vol], [rightvol], [rate])
- [ ] sound.sample:save(filename)
- [ ] sound.signal:setOffset(offset)
- [ ] sound.signal:setScale(scale)
- [ ] sound.signal:getValue()
- [ ] sound.channel.new()
- [ ] sound.channel:remove()
- [ ] sound.channel:addEffect(effect)
- [ ] sound.channel:removeEffect(effect)
- [ ] sound.channel:addSource(source)
- [ ] sound.channel:removeSource(source)
- [ ] sound.channel:setVolume(volume)
- [ ] sound.channel:getVolume()
- [ ] sound.channel:setPan(pan)
- [ ] sound.channel:setPanMod(signal)
- [ ] sound.channel:setVolumeMod(signal)
- [ ] sound.channel:getDryLevelSignal()
- [ ] sound.channel:getWetLevelSignal()
- [ ] sound.playingSources() - Return a list of sources playing
- [x] sound.getCurrentTime() - Returns the current time, in seconds, as measured by the audio device
- [x] sound.resetTime() - Resets the audio output device time counter.

### MP3 Player (using picomp3lib)

- [x] sound.mp3player.new() - Create MP3 player instance
- [x] sound.mp3player:load(path) - Load MP3 file
- [x] sound.mp3player:play([repeatCount]) - Start playback
- [x] sound.mp3player:stop() - Stop playback
- [x] sound.mp3player:pause() - Pause playback
- [x] sound.mp3player:resume() - Resume playback
- [x] sound.mp3player:isPlaying() - Check if playing
- [x] sound.mp3player:getPosition() - Get current position in samples
- [x] sound.mp3player:getLength() - Get total length in samples
- [x] sound.mp3player:setVolume(vol) - Set volume (0-100)
- [x] sound.mp3player:getVolume() - Get current volume
- [x] sound.mp3player:setLoop(flag) - Enable/disable looping

---

## WiFi — implemented

Hardware: CYW43 on Pimoroni Pico Plus 2 W — **shares SPI1 with the LCD**.
Must use `display_spi_lock()` / `display_spi_unlock()` around all CYW43 SPI access.

- [x] Create `src/drivers/wifi.c` / `wifi.h`
- [x] `wifi_init()` — CYW43 init via `cyw43_arch_init()`, SPI mutex enabled in `display_flush()`, auto-connects from config
- [x] `wifi_connect(ssid, password)` — non-blocking; uses `cyw43_arch_wifi_connect_async()` with WPA2_MIXED_PSK
- [x] `wifi_get_status()` — returns `WIFI_STATUS_*` enum (defined in `os.h`)
- [x] `wifi_disconnect()` — calls `cyw43_wifi_leave()`
- [x] `wifi_get_ip()` / `wifi_get_ssid()` — lwip netif for IP
- [x] `wifi_is_available()` — tracks `cyw43_arch_init()` success at runtime
- [x] Expose full `picocalc.wifi.*` table to Lua with `STATUS_*` constants
- [x] Wire `g_api.wifi` in `main.c`
- [x] `pico_cyw43_arch_lwip_poll` added to `CMakeLists.txt` (conditional on WiFi-capable boards)
- [x] `wifi_poll()` called from Lua instruction hook every ~256 opcodes (background polling)

---

## System menu overlay — implemented

The Menu/Sym key (`BTN_MENU`) pauses the running app and shows an OS-level overlay.

- [x] Create `src/os/system_menu.c` / `system_menu.h`
- [x] Intercept `BTN_MENU` via Lua instruction-count hook (fires every 256 opcodes) + `kbd_consume_menu_press()` edge-detect in `keyboard.c`
- [x] Draw a translucent overlay via `display_darken()` + menu panel
- [x] Built-in items: **Brightness** (L/R/Enter adjust), **Battery** (colour-coded %), **WiFi** (stub), **Reboot**, **Exit app**
- [x] `system_menu_add_item()` / `system_menu_clear_items()` for app-registered items
- [x] Wire `g_api.sys.addMenuItem` and `clearMenuItems` in `main.c`
- [x] Expose `picocalc.sys.addMenuItem(label, fn)` and `clearMenuItems()` to Lua
- [x] Menu button also works during `sys.sleep()` (10ms polling loop)
- [ ] Implement the system menu overlay working on the main menu

---

## Display — missing features

- [ ] `display.drawBitmap(x, y, path)` — load a raw RGB565 or BMP file from SD card and blit it
- [ ] Larger/alternative font support — current 6×8 font is readable but small; add an 8×12 option
- [ ] `display.drawCircle(x, y, r, color)` — useful for apps, trivial to add
- [ ] `display.scroll(dy)` — hardware-assisted vertical scroll (ST7365P supports this)

---

## System — miscellaneous

- [ ] **Core 1 background tasks** — `core1_entry()` in `main.c` is an idle spin loop; candidates: audio mixing, WiFi polling, display DMA coordination
- [x] **Shared config** — `src/os/config.h` / `config.c`; reads/writes `/system/config.json`; flat key/value JSON; exposed as `picocalc.config.{get,set,save,load}()`
- [ ] **`sys.isUSBPowered()` implementation** — currently a stub; on RP2350 Pico, VBUS is detectable via GP24
- [x] **SD card hot-swap** — `sdcard_remount()` exists; launcher rescans on app exit; manual "Remount SD" added to system menu.

---

## System — Filesystem handling

- [x] Restrict files opened to either their own app directory or their own data directory
- [x] Add a file browser in similar styling to the system menu

---

## Code quality / housekeeping

- [ ] `g_api.fs.listDir` callback signature mismatch — `picocalc_fs_t.listDir` in `os.h` has a different signature than `sdcard_list_dir()` in `sdcard.h`; reconcile them (Lua bridge calls `sdcard_list_dir()` directly and is unaffected)
- [ ] App sandboxing: `picocalc.sys.exit()` uses a string sentinel (`__picocalc_exit__`) — could be hardened with a Lua registry light userdata instead

## Quality of life

- [ ] Remember the position of the selected app/scrolling position when exiting an application
- [ ] Add a loading spinner api that apps can use

## Graphics APIs - For Lua

- [done] Basic Context & State Management (These define how the MCU interacts with the frame buffer.)
  - [x] graphics.clear(color): Wipes the current draw target.
  - [x] graphics.setColor(color) / graphics.setBackgroundColor(color): Sets global draw state.

- [done] Image Lifecycle & Memory (MCUs often need to distinguish between images stored in Flash (ROM) vs. those in RAM.)
  - [x] graphics.image.new: Allocates a buffer in RAM.
  - [x] graphics.image.load(path): Loads from SD card or SPIFFS.
  - [x] graphics.image:getSize(): Crucial for bounds checking.
  - [x] graphics.image:copy(): Clones an image buffer (use sparingly due to RAM).

- [done] Coordinate-Based Drawing (These are the workhorses of your UI.)
  - [x] graphics.image:draw(x, y, [flip, sourceRect]): Standard blitting.
  - [x] graphics.image:drawAnchored(x, y, ax, ay): Simplifies UI alignment (e.g., centering text or icons).
  - [x] graphics.image:drawTiled(rect): Great for backgrounds without using massive assets.

## Optimisations

- [ ] Route direct hardware calls in `lua_bridge.c` through g_api for environment portability (Medium effort)
- [ ] Split `lua_bridge.c` into ~9 files along existing section boundaries
- [ ] Replace tight_loop_contents() on Core 1 with __wfe() for power savings
- [ ] Fix `display_draw_image_scaled()` to only byte-swap the affected region
- [ ] Consider storing framebuffer in native endian, byte-swap only during DMA	
- [ ] Disable Wifi after getting time, applications should be able to request the connection
- [ ] Skip wifi_poll() in lua hook when WiFi is disabled