# API System and Config

System-level functions and persistent configuration storage.

## picocalc.sys

### Functions

#### `picocalc.sys.getTimeMs()`
Returns milliseconds since boot.

- **Parameters:** None
- **Returns:** (number) Milliseconds since system startup

```lua
local start = picocalc.sys.getTimeMs()
-- do work
local elapsed = picocalc.sys.getTimeMs() - start
```

---

#### `picocalc.sys.sleep(ms)`
Sleeps for the specified number of milliseconds. Does not consume input events.

- **Parameters:**
  - `ms` (number): Milliseconds to sleep
- **Returns:** None

```lua
picocalc.sys.sleep(100)  -- Sleep for 100ms
```

---

#### `picocalc.sys.getBattery()`
Returns the battery charge level. Cached for 5 seconds to avoid slow I²C reads.

- **Parameters:** None
- **Returns:** (number) Battery percentage (0-100), or -1 if unknown/USB powered

```lua
local battery = picocalc.sys.getBattery()
if battery >= 0 then
    print("Battery: " .. battery .. "%")
end
```

---

#### `picocalc.sys.isUSBPowered()`
Checks if the device is powered via USB (GP24 VBUS sense).

- **Parameters:** None
- **Returns:** (boolean) `true` when USB power is connected

---

#### `picocalc.sys.resetIdleTimer()`
Resets the idle screen-dim timer. Call on user activity to keep the display from dimming (see the `dim_timeout_s` key in [[API Sysconfig]]).

- **Parameters:** None
- **Returns:** None

```lua
if pressed ~= 0 then
    picocalc.sys.resetIdleTimer()
end
```

---

#### `picocalc.sys.log(message)`
Logs a message to the USB serial debug output (115200 baud).

- **Parameters:**
  - `message` (string): Message to log
- **Returns:** None

```lua
picocalc.sys.log("Debug info: x=" .. tostring(x))
```

---

#### `picocalc.sys.exit()`
Exits the current app cleanly and returns to the launcher. Works from any call depth.

- **Parameters:** None
- **Returns:** Never returns

```lua
picocalc.sys.exit()
```

---

#### `picocalc.sys.reboot()`
Triggers a system reboot via the watchdog timer.

- **Parameters:** None
- **Returns:** Never returns

```lua
picocalc.sys.reboot()
```

---

#### `picocalc.sys.getClock()`
Returns the current time as a table. Time is synchronized via NTP when WiFi is connected.

- **Parameters:** None
- **Returns:** (table) Clock data with fields:
  - `synced` (boolean): `true` if time has been synchronized via NTP
  - `hour` (number): Current hour (0-23, adjusted for timezone)
  - `min` (number): Current minute (0-59)
  - `sec` (number): Current second (0-59)
  - `epoch` (number): UTC Unix timestamp in seconds

```lua
local clock = picocalc.sys.getClock()
if clock.synced then
    local time_str = string.format("%02d:%02d:%02d", clock.hour, clock.min, clock.sec)
    picocalc.display.drawText(10, 10, time_str, picocalc.display.WHITE)
end
```

---

#### `picocalc.sys.getMemInfo()`
Returns a snapshot of heap memory usage.

- **Parameters:** None
- **Returns:** (table) with fields:
  - `psram_free` (number): Free bytes in the Lua PSRAM heap
  - `psram_used` (number): Used bytes in the Lua PSRAM heap
  - `psram_total` (number): Total bytes in the Lua PSRAM heap
  - `sram_free` (number): Free bytes in the system SRAM heap (via `mallinfo`)
  - `sram_used` (number): Used bytes in the system SRAM heap

```lua
local mem = picocalc.sys.getMemInfo()
picocalc.sys.log(string.format("PSRAM: %dKB free / %dKB total",
    mem.psram_free // 1024, mem.psram_total // 1024))
```

---

#### `picocalc.sys.addMenuItem(label, callback)`
Adds a custom item to the system menu overlay (Menu key). Maximum **4 items per app**.

- **Parameters:**
  - `label` (string): Menu item text
  - `callback` (function): Function to call when the item is selected
- **Returns:** None

```lua
picocalc.sys.addMenuItem("Restart Level", function()
    level = 1
end)
```

---

#### `picocalc.sys.clearMenuItems()`
Removes all app-registered menu items. Called automatically on app exit.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.sys.triggerFault()`
Deliberately triggers a HardFault by writing to address 0. **For crash-handler testing only — never call in production code.**

The fault handler will display register state on screen, save crash data via watchdog scratch registers, then reboot. On the next boot the crash data is written to `/system/crashlog.txt`.

- **Parameters:** None
- **Returns:** Never returns

```lua
-- Test that crash logging works
picocalc.sys.triggerFault()
```

---

#### `picocalc.sys.getVersion()`
Get the PicOS firmware version string.

- **Parameters:** None
- **Returns:** (string) Version string (e.g. "1.2.0")

```lua
local version = picocalc.sys.getVersion()
picocalc.sys.log("PicOS version: " .. version)
```

---

#### `picocalc.sys.getPowerStatus()`
Get detailed power status.

- **Parameters:** None
- **Returns:** (table) With fields:
  - `charging` (boolean): Whether the device is currently charging
  - `percent` (number): Battery percentage (0-100)

```lua
local power = picocalc.sys.getPowerStatus()
if power.charging then
    print("Charging: " .. power.percent .. "%")
end
```

---

#### `picocalc.sys.applyUpdate(path)`
Trigger an OTA firmware update from a UF2 file on the SD card. The device will reboot to apply the update.

- **Parameters:**
  - `path` (string): Path to UF2 file on the SD card
- **Returns:** (boolean) `true` on success; `false, string` on failure with error message

```lua
local ok, err = picocalc.sys.applyUpdate("/system/update.uf2")
if not ok then
    picocalc.sys.log("Update failed: " .. err)
end
```

---

#### `picocalc.sys.pauseBackground()`
Pause Core 1 background tasks (WiFi polling, audio decode, HTTP). Useful before intensive SD card operations.

- **Parameters:** None
- **Returns:** (boolean) `true` if Core 1 successfully paused

```lua
picocalc.sys.pauseBackground()
-- perform intensive SD card operations
picocalc.sys.resumeBackground()
```

---

#### `picocalc.sys.resumeBackground()`
Resume Core 1 background tasks after `pauseBackground()`.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.sys.resumeBackground()
```

---

#### `picocalc.sys.loadlib(name)`
Load a shared Lua library from `/system/lib/<name>.lua` and return its result. Libraries are standard Lua files that return a table of functions.

- **Parameters:**
  - `name` (string): Library name (without `.lua` extension)
- **Returns:** Whatever the library script returns (typically a table)

```lua
local json = picocalc.sys.loadlib("json")
local data = json.decode(raw)
```

---

#### `picocalc.sys.pioPsramRead(addr, len)`
Read bytes from PIO PSRAM (mainboard 8MB).

- **Parameters:**
  - `addr` (number): Byte address
  - `len` (number): Number of bytes to read
- **Returns:** (string) Data, or `nil` if PIO PSRAM not available

```lua
local data = picocalc.sys.pioPsramRead(0x0000, 256)
```

---

#### `picocalc.sys.pioPsramWrite(addr, data)`
Write bytes to PIO PSRAM.

- **Parameters:**
  - `addr` (number): Byte address
  - `data` (string): Bytes to write
- **Returns:** (number) Bytes written (0 if unavailable)

```lua
local written = picocalc.sys.pioPsramWrite(0x8000, myData)
```

---

#### `picocalc.sys.pioPsramSize()`
Get PIO PSRAM size.

- **Parameters:** None
- **Returns:** (number) Size in bytes (0 if not available)

```lua
local size = picocalc.sys.pioPsramSize()
if size > 0 then
    picocalc.sys.log("PIO PSRAM: " .. (size // 1024) .. " KB")
end
```

---

#### `picocalc.sys.qmiPsramAlloc(size)`
Allocate a buffer in QMI PSRAM (Lua heap). Low-level; prefer standard Lua tables for most uses.

- **Parameters:**
  - `size` (number): Bytes to allocate
- **Returns:** (lightuserdata) Handle, or `nil` on failure

```lua
local buf = picocalc.sys.qmiPsramAlloc(4096)
```

---

#### `picocalc.sys.qmiPsramFree(handle)`
Free a QMI PSRAM allocation.

- **Parameters:**
  - `handle` (lightuserdata): Handle from `qmiPsramAlloc`
- **Returns:** None

```lua
picocalc.sys.qmiPsramFree(buf)
```

---

#### `picocalc.sys.qmiPsramWrite(handle, offset, data)`
Write to a QMI PSRAM buffer.

- **Parameters:**
  - `handle` (lightuserdata): Handle from `qmiPsramAlloc`
  - `offset` (number): Byte offset within the buffer
  - `data` (string): Bytes to write
- **Returns:** (number) Bytes written

```lua
picocalc.sys.qmiPsramWrite(buf, 0, "Hello PSRAM")
```

---

#### `picocalc.sys.qmiPsramRead(handle, offset, len)`
Read from a QMI PSRAM buffer.

- **Parameters:**
  - `handle` (lightuserdata): Handle from `qmiPsramAlloc`
  - `offset` (number): Byte offset within the buffer
  - `len` (number): Number of bytes to read
- **Returns:** (string) Data

```lua
local data = picocalc.sys.qmiPsramRead(buf, 0, 11)
```

---

## picocalc.config

Persistent **per-app** key-value configuration storage, stored at `/data/<APP_ID>/config.json`. Each app gets its own isolated store. The same store is also available under the alias `picocalc.appconfig` — same data, two names.

For the **system-wide** store shared by all apps (`/system/config.json`), use `picocalc.sysconfig` — see [[API Sysconfig]].

### Functions

#### `picocalc.config.get(key)`
Retrieves a configuration value.

- **Parameters:**
  - `key` (string): Configuration key
- **Returns:** (string or nil) Value, or `nil` if key does not exist

```lua
local highscore = picocalc.config.get("highscore")
```

---

#### `picocalc.config.set(key [, value])`
Sets a configuration value. Pass `nil` as value to delete the key.

- **Parameters:**
  - `key` (string): Configuration key
  - `value` (string or nil): Value to store, or `nil` to delete
- **Returns:** None

```lua
picocalc.config.set("highscore", "1000")
picocalc.config.set("old_key", nil)  -- Delete key
```

---

#### `picocalc.config.save()`
Saves the current configuration to `/data/<APP_ID>/config.json`.

- **Parameters:** None
- **Returns:** (boolean) `true` on success, `false` on error

```lua
if picocalc.config.save() then
    print("Config saved")
end
```

---

#### `picocalc.config.load()`
Loads configuration from `/data/<APP_ID>/config.json`.

- **Parameters:** None
- **Returns:** (boolean) `true` on success, `false` on error

```lua
picocalc.config.load()
```

---

#### `picocalc.config.clear()`
Clears all keys from the current app's configuration in memory. Does not delete the config file on disk; call `save()` afterwards to persist the change.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.config.clear()
picocalc.config.save()  -- persist the empty config
```

---

#### `picocalc.config.reset()`
Resets the app configuration by deleting the config file from the SD card and clearing the in-memory state.

- **Parameters:** None
- **Returns:** (boolean) `true` if the file was deleted successfully, `false` on error

```lua
if picocalc.config.reset() then
    picocalc.sys.log("App config reset to defaults")
end
```
