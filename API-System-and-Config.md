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
Checks if the device is powered via USB.

- **Parameters:** None
- **Returns:** (boolean) Currently always returns `false` (stub implementation)

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

## picocalc.config

Persistent key-value configuration storage (stored in `/system/config.json`).

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
Saves the current configuration to `/system/config.json`.

- **Parameters:** None
- **Returns:** (boolean) `true` on success, `false` on error

```lua
if picocalc.config.save() then
    print("Config saved")
end
```

---

#### `picocalc.config.load()`
Loads configuration from `/system/config.json`.

- **Parameters:** None
- **Returns:** (boolean) `true` on success, `false` on error

```lua
picocalc.config.load()
```
