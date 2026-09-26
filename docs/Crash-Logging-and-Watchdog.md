# Crash Logging and Watchdog

PicOS captures two categories of failure: HardFaults (CPU exceptions) and Lua runtime errors. Both are written to the SD card so they survive a reboot and can be inspected later.

---

## HardFault recovery flow

1. **Fault fires** — the `isr_hardfault` / `hardfault_c` handler runs on Core 0.
2. **Display** — fault details (PC, LR, SP, CFSR/HFSR/BFAR, decoded flag names) are printed to both the LCD and UART so you can read them without a serial adapter.
3. **Scratch registers** — the raw fault frame is encoded into RP2350 watchdog scratch registers (`SCRATCH4`/`SCRATCH5`) so the data survives the upcoming reset.
4. **Reboot** — after a ~2-second display pause the handler calls `watchdog_reboot(0, 0, 0)`.
5. **Next boot** — once PSRAM is initialised and the SD card is mounted, `crash_log_save()` reads the scratch registers and appends a record to `/system/crashlog.txt`.

> **Why the delay before `crash_log_save()`?**
> `sdcard_fopen()` calls `umm_malloc()` internally. The PSRAM heap (umm) is not ready until `lua_psram_alloc_init()` has run, so the save is intentionally deferred to that point in the boot sequence.

---

## Log files

| File | Contents | Max size |
|------|----------|----------|
| `/system/crashlog.txt` | HardFault records written on the boot after a crash | 64 KB (truncated on open if larger) |
| `/system/error.log` | Lua runtime errors recorded during normal execution | 64 KB (truncated on open if larger) |
| `/system/filesystem.log` | SD card / FatFS structural errors (disk errors, media failures) | 4 KB (truncated on open if larger) |

All files are opened in **append** mode so multiple events accumulate. They are truncated (not rotated) when they exceed their size limit.

---

## Filesystem corruption logging

Hardware-level SD card errors (distinct from benign "file not found" results) are automatically appended to `/system/filesystem.log`. This covers:

| FatFS result | Meaning |
|---|---|
| `FR_DISK_ERR` (1) | Low-level I/O error from the SPI layer |
| `FR_INT_ERR` (2) | Internal FatFS assertion failure |
| `FR_NOT_READY` (3) | Physical drive not ready |
| `FR_NO_FILESYSTEM` (13) | No valid FAT/exFAT volume found |
| `FR_INVALID_DRIVE` (11) | Invalid drive number |
| `FR_MKFS_ABORTED` (14) | mkfs aborted |
| `FR_TIMEOUT` (15) | Mutex timeout |
| `FR_LOCKED` (16) | Object access locked |
| `FR_NOT_ENOUGH_CORE` (17) | Not enough memory |
| `FR_TOO_MANY_OPEN_FILES` (18) | Too many open files |

Each log entry includes a timestamp (ms since boot), the FatFS error code, the call site (e.g. `sdcard_fopen`, `sdcard_init`), and the path if applicable:

```
[1234ms] FS ERROR: FRESULT=1 (disk err) at sdcard_fopen path=/apps/myapp/main.lua
```

This is written at: `sdcard_init`, `sdcard_remount`, `sdcard_fopen`, `sdcard_fread`, `sdcard_fwrite`, and `sdcard_list_dir` (both `opendir` and `readdir` phases).

---

## Watchdog timeout

The watchdog is enabled with a **10-second** timeout (`watchdog_enable(10000, true)`). The boot sequence calls `watchdog_update()` at several checkpoints (after PSRAM init, after SD mount, after config load, etc.) to prevent a spurious reset during a slow cold boot.

`watchdog_caused_reboot()` is printed to UART at each boot so you can distinguish a normal power-on from a watchdog-triggered reset.

---

## Testing the crash handler

`picocalc.sys.triggerFault()` writes to address 0, which triggers an immediate bus fault. Use it from a Lua app to verify that the full crash→reboot→log cycle works:

```lua
-- apps/crashtest/main.lua
picocalc.sys.log("About to trigger fault...")
picocalc.sys.triggerFault()
-- device will reboot; check /system/crashlog.txt afterwards
```

---

## Lua error logging

Lua runtime errors are written to `/system/error.log` by `crashlog_write_lua_error()` (C API). This is called automatically by the Lua runner when a `lua_pcall()` fails. The record includes the app name, error context, and the full error message.

Format:
```
--- LUA ERROR [com.example.myapp] ---
runtime error
attempt to index a nil value (global 'foo')
```
