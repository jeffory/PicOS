# PicOS TODO

Status key: **[done]** implemented and working · **[partial]** exists in C but not exposed to Lua · **[missing]** not implemented at all · **[pending]** waiting on test results

---

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

`g_api.fs`, `g_api.audio`, and `g_api.wifi` are never assigned in `main.c` (left NULL).
The Lua bridge calls C functions directly so Lua is unaffected, but the C app loader
(future) will need these populated.

- [ ] Wire `g_api.fs = &s_fs_impl` once a `picocalc_fs_t` struct is built
- [ ] Wire `g_api.audio` once audio is implemented
- [ ] Wire `g_api.wifi` once WiFi is implemented

---

## Audio — not implemented

Driver location: `src/drivers/audio.c` (to be created)
Hardware: PWM on GP26 (left) and GP27 (right) — defined in `hardware.h`

- [ ] Create `src/drivers/audio.h` / `audio.c`
- [ ] `audio_init()` — configure PWM slices on GP26/GP27
- [ ] `audio_play_tone(uint32_t freq_hz, uint32_t duration_ms)` — square wave; 0ms = indefinite
- [ ] `audio_stop_tone()`
- [ ] `audio_set_volume(uint8_t vol)` — 0–100, scales PWM duty cycle
- [ ] Add `picocalc.audio.playTone(freq, duration)`, `stopTone()`, `setVolume(vol)` to Lua bridge
- [ ] Wire `g_api.audio` in `main.c`

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

## Keyboard — pending test results

Run the **Key Test** app (`/apps/keytest`) and check what raw hex codes appear for each key.

- [ ] **F1–F10 keys** — note the hex codes from the Key Test log and add `KEY_F1`–`KEY_F10` constants to `keyboard.h`, `BTN_F1`–`BTN_F10` to `os.h`, and wire them in `kbd_poll()`
- [ ] Once F-key codes are known, expose `picocalc.input.BTN_F1` … `BTN_F10` constants to Lua

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