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
| `fs.appPath(name)` | **[missing]** | Convenience: prepend `/data/APP_NAME/` to a filename; auto-create the directory |

### `picocalc.display`

| Function | Status | Notes |
|---|---|---|
| `display.textWidth(str)` | **[done]** | `l_display_textWidth` wrapper added to `l_display_lib` |

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

## WiFi — not implemented

Hardware: CYW43 on Pimoroni Pico Plus 2 W — **shares SPI1 with the LCD**.
Must use `display_spi_lock()` / `display_spi_unlock()` around all CYW43 SPI access.

- [ ] Create `src/drivers/wifi.c` / `wifi.h`
- [ ] `wifi_init()` — CYW43 init via `cyw43_arch_init()`, register SPI arbitration with display mutex
- [ ] `wifi_connect(ssid, password)` — non-blocking; use `cyw43_arch_wifi_connect_async()`
- [ ] `wifi_get_status()` — returns `WIFI_STATUS_*` enum (defined in `os.h`)
- [ ] `wifi_disconnect()`
- [ ] `wifi_get_ip()` / `wifi_get_ssid()`
- [ ] `wifi_is_available()` — detect Pico 2W vs standard Pico 2 at runtime
- [ ] Expose full `picocalc.wifi.*` table to Lua (see `os.h` for the full interface)
- [ ] Wire `g_api.wifi` in `main.c`
- [ ] Add `pico_cyw43_arch` to `CMakeLists.txt` (already listed in SDK but needs enabling)

---

## System menu overlay — not implemented

The Menu/Sym key (`BTN_MENU`) should pause the running app and show an OS-level overlay.

- [ ] Create `src/os/system_menu.c` / `system_menu.h`
- [ ] Intercept `BTN_MENU` in the app run loop (needs a C-level hook since Lua can't be interrupted)
- [ ] Draw a translucent overlay over the current framebuffer
- [ ] Built-in items: **Brightness**, **Battery**, **WiFi status**, **Reboot**, **Exit app**
- [ ] `addMenuItem(label, callback, user)` / `clearMenuItems()` for app-registered items
- [ ] Wire `g_api.sys.addMenuItem` and `clearMenuItems` in `main.c` (currently NULL)
- [ ] Expose `picocalc.sys.addMenuItem(label, fn)` to Lua

---

## Display — missing features

- [ ] `display.drawBitmap(x, y, path)` — load a raw RGB565 or BMP file from SD card and blit it
- [ ] Larger/alternative font support — current 6×8 font is readable but small; add an 8×12 option
- [ ] `display.drawCircle(x, y, r, color)` — useful for apps, trivial to add
- [ ] `display.scroll(dy)` — hardware-assisted vertical scroll (ST7365P supports this)

---

## Keyboard — pending test results

Run the **Key Test** app (`/apps/keytest`) and check what raw hex codes appear for each key.

- [ ] **Sym key as BTN_MENU** — verify that pressing Sym produces keycode `0xA4` in the event log; if nothing appears, the STM32 firmware has `CFG_REPORT_MODS` disabled and we need a different key
- [ ] **F1–F10 keys** — note the hex codes from the Key Test log and add `KEY_F1`–`KEY_F10` constants to `keyboard.h`, `BTN_F1`–`BTN_F10` to `os.h`, and wire them in `kbd_poll()`
- [ ] Once F-key codes are known, expose `picocalc.input.BTN_F1` … `BTN_F10` constants to Lua

---

## System — miscellaneous

- [ ] **Restore 200 MHz overclock** — `set_sys_clock_khz(200000, true)` is commented out in `main.c` pending keyboard reliability confirmation; re-enable once keyboard is stable
- [ ] **Core 1 background tasks** — `core1_entry()` in `main.c` is an idle spin loop; candidates: audio mixing, WiFi polling, display DMA coordination
- [ ] **Shared config** — read/write `/system/config.json` for persisted settings (WiFi credentials, brightness, etc.)
- [ ] **`sys.isUSBPowered()` implementation** — currently a stub; on RP2350 Pico, VBUS is detectable via GP24
- [ ] **SD card hot-swap** — `sdcard_remount()` exists; launcher rescans on app exit, but in-app remount notification is unhandled

---

## Code quality / housekeeping

- [ ] Remove verbose `[KBD]` init logging from `keyboard.c` once keyboard is confirmed stable (or gate it behind a `#define KBD_DEBUG`)
- [ ] `g_api.fs.listDir` callback signature mismatch — `picocalc_fs_t.listDir` in `os.h` has a different signature than `sdcard_list_dir()` in `sdcard.h`; reconcile them (Lua bridge calls `sdcard_list_dir()` directly and is unaffected)
- [ ] App sandboxing: `picocalc.sys.exit()` uses a string sentinel (`__picocalc_exit__`) — could be hardened with a Lua registry light userdata instead
