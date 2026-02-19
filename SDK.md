# PicOS Lua SDK Reference

This document provides a complete reference for all Lua APIs available to PicOS apps.

## Table of Contents

- [Global Variables](#global-variables)
- [picocalc.display](#picocalcdisplay) — Graphics & Display
- [picocalc.input](#picocalcinput) — Keyboard & Button Input
- [picocalc.sys](#picocalcsys) — System Functions
- [picocalc.fs](#picocalcfs) — Filesystem (SD Card)
- [picocalc.wifi](#picocalcwifi) — WiFi Connectivity
- [picocalc.config](#picocalcconfig) — Persistent Configuration
- [picocalc.perf](#picocalcperf) — Performance Monitoring
- [Standard Lua Libraries](#standard-lua-libraries)

---

## Global Variables

These variables are automatically set when your app is launched:

| Variable | Type | Description |
|----------|------|-------------|
| `APP_DIR` | string | Absolute path to your app's directory (e.g., `"/apps/hello"`) |
| `APP_NAME` | string | Name of your app as defined in `app.json` |
| `APP_ID` | string | Reverse-DNS app identifier from `app.json` (e.g., `"com.picos.hello"`) |

---

## picocalc.display

Graphics and display functions. The display is **320×320 pixels** with RGB565 color format.

### Functions

#### `picocalc.display.clear([color])`
Clears the entire framebuffer to the specified color.

- **Parameters:**
  - `color` (number, optional): RGB565 color value. Defaults to `BLACK` if omitted.
- **Returns:** None

```lua
picocalc.display.clear(picocalc.display.BLACK)
```

---

#### `picocalc.display.setPixel(x, y, color)`
Sets a single pixel at the specified coordinates.

- **Parameters:**
  - `x` (number): X coordinate (0-319)
  - `y` (number): Y coordinate (0-319)
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.setPixel(160, 160, picocalc.display.WHITE)
```

---

#### `picocalc.display.fillRect(x, y, width, height, color)`
Draws a filled rectangle.

- **Parameters:**
  - `x` (number): Top-left X coordinate
  - `y` (number): Top-left Y coordinate
  - `width` (number): Rectangle width in pixels
  - `height` (number): Rectangle height in pixels
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.fillRect(10, 10, 50, 30, picocalc.display.RED)
```

---

#### `picocalc.display.drawRect(x, y, width, height, color)`
Draws a rectangle outline (1-pixel border).

- **Parameters:**
  - `x` (number): Top-left X coordinate
  - `y` (number): Top-left Y coordinate
  - `width` (number): Rectangle width in pixels
  - `height` (number): Rectangle height in pixels
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.drawRect(10, 10, 100, 50, picocalc.display.BLUE)
```

---

#### `picocalc.display.drawLine(x0, y0, x1, y1, color)`
Draws a line between two points.

- **Parameters:**
  - `x0` (number): Starting X coordinate
  - `y0` (number): Starting Y coordinate
  - `x1` (number): Ending X coordinate
  - `y1` (number): Ending Y coordinate
  - `color` (number): RGB565 color value
- **Returns:** None

```lua
picocalc.display.drawLine(0, 0, 319, 319, picocalc.display.GREEN)
```

---

#### `picocalc.display.drawText(x, y, text, fg_color [, bg_color])`
Draws text using the built-in 6×8 pixel bitmap font (ASCII 0x20–0x7E).

- **Parameters:**
  - `x` (number): Top-left X coordinate
  - `y` (number): Top-left Y coordinate
  - `text` (string): Text to draw
  - `fg_color` (number): Foreground RGB565 color
  - `bg_color` (number, optional): Background RGB565 color. Defaults to `BLACK`.
- **Returns:** (number) Pixel width of the drawn text

```lua
local width = picocalc.display.drawText(10, 10, "Hello!", picocalc.display.WHITE)
```

---

#### `picocalc.display.textWidth(text)`
Calculates the pixel width of text without drawing it.

- **Parameters:**
  - `text` (string): Text to measure
- **Returns:** (number) Width in pixels

```lua
local width = picocalc.display.textWidth("Hello World")
```

---

#### `picocalc.display.flush()`
Flushes the internal framebuffer to the LCD via DMA. **Call once per frame** after all drawing is complete.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.display.flush()
```

---

#### `picocalc.display.getWidth()`
Returns the display width in pixels.

- **Parameters:** None
- **Returns:** (number) 320

---

#### `picocalc.display.getHeight()`
Returns the display height in pixels.

- **Parameters:** None
- **Returns:** (number) 320

---

#### `picocalc.display.setBrightness(level)`
Sets the display backlight brightness.

- **Parameters:**
  - `level` (number): Brightness value (0-255, where 255 is full brightness)
- **Returns:** None

```lua
picocalc.display.setBrightness(128)  -- 50% brightness
```

---

#### `picocalc.display.rgb(r, g, b)`
Converts 8-bit RGB components to a 16-bit RGB565 color value.

- **Parameters:**
  - `r` (number): Red component (0-255)
  - `g` (number): Green component (0-255)
  - `b` (number): Blue component (0-255)
- **Returns:** (number) RGB565 color value

```lua
local purple = picocalc.display.rgb(128, 0, 128)
picocalc.display.clear(purple)
```

---

### Color Constants

Predefined RGB565 color values:

| Constant | Color |
|----------|-------|
| `picocalc.display.BLACK` | Black (0, 0, 0) |
| `picocalc.display.WHITE` | White (255, 255, 255) |
| `picocalc.display.RED` | Red (255, 0, 0) |
| `picocalc.display.GREEN` | Green (0, 255, 0) |
| `picocalc.display.BLUE` | Blue (0, 0, 255) |
| `picocalc.display.YELLOW` | Yellow (255, 255, 0) |
| `picocalc.display.CYAN` | Cyan (0, 255, 255) |
| `picocalc.display.GRAY` | Gray (128, 128, 128) |

---

## picocalc.input

Keyboard and button input functions.

### Functions

#### `picocalc.input.update()`
Polls the keyboard for new input events. **Call once per frame** before reading button or character state.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.input.update()
```

---

#### `picocalc.input.getButtons()`
Returns the current bitmask of **held** buttons.

- **Parameters:** None
- **Returns:** (number) Bitmask of currently pressed buttons

```lua
local buttons = picocalc.input.getButtons()
if buttons & picocalc.input.BTN_UP ~= 0 then
    -- Up button is held
end
```

---

#### `picocalc.input.getButtonsPressed()`
Returns the bitmask of buttons that were **pressed this frame** (edge detection, not held).

- **Parameters:** None
- **Returns:** (number) Bitmask of buttons pressed this frame

```lua
local pressed = picocalc.input.getButtonsPressed()
if pressed & picocalc.input.BTN_ENTER ~= 0 then
    -- Enter was just pressed
end
```

---

#### `picocalc.input.getButtonsReleased()`
Returns the bitmask of buttons that were **released this frame**.

- **Parameters:** None
- **Returns:** (number) Bitmask of buttons released this frame

---

#### `picocalc.input.getChar()`
Returns the last ASCII character typed, if any.

- **Parameters:** None
- **Returns:** (string or nil) Single-character string, or `nil` if no character was typed this frame

```lua
local ch = picocalc.input.getChar()
if ch then
    text = text .. ch
end
```

---

#### `picocalc.input.getRawKey()`
Returns the raw STM32 keycode from the keyboard controller.

- **Parameters:** None
- **Returns:** (number) Raw key code

---

### Button Constants

Bitmask values for button states:

| Constant | Description |
|----------|-------------|
| `picocalc.input.BTN_UP` | D-pad Up |
| `picocalc.input.BTN_DOWN` | D-pad Down |
| `picocalc.input.BTN_LEFT` | D-pad Left |
| `picocalc.input.BTN_RIGHT` | D-pad Right |
| `picocalc.input.BTN_ENTER` | Enter key |
| `picocalc.input.BTN_ESC` | Escape key |
| `picocalc.input.BTN_MENU` | Menu key (system overlay, auto-handled) |
| `picocalc.input.BTN_F1` | F1 key |
| `picocalc.input.BTN_F2` | F2 key |
| `picocalc.input.BTN_F3` | F3 key |
| `picocalc.input.BTN_F4` | F4 key |
| `picocalc.input.BTN_F5` | F5 key |
| `picocalc.input.BTN_F6` | F6 key |
| `picocalc.input.BTN_F7` | F7 key |
| `picocalc.input.BTN_F8` | F8 key |
| `picocalc.input.BTN_F9` | F9 key |
| `picocalc.input.BTN_BACKSPACE` | Backspace key |
| `picocalc.input.BTN_TAB` | Tab key |
| `picocalc.input.BTN_DEL` | Delete key |
| `picocalc.input.BTN_SHIFT` | Shift modifier (left or right) |
| `picocalc.input.BTN_CTRL` | Ctrl modifier |
| `picocalc.input.BTN_ALT` | Alt modifier |
| `picocalc.input.BTN_FN` | Fn/Symbol modifier |

---

## picocalc.sys

System-level functions for timing, logging, and app control.

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

## picocalc.fs

Filesystem access to the SD card (FAT32).

### Functions

#### `picocalc.fs.open(path [, mode])`
Opens a file on the SD card.

- **Parameters:**
  - `path` (string): Absolute file path (e.g., `"/apps/hello/data.txt"`)
  - `mode` (string, optional): File mode (`"r"`, `"w"`, `"a"`, `"rb"`, `"wb"`, etc.). Defaults to `"r"`.
- **Returns:** (userdata or nil) File handle, or `nil` on error

```lua
local f = picocalc.fs.open("/data/save.txt", "w")
if f then
    picocalc.fs.write(f, "Hello")
    picocalc.fs.close(f)
end
```

---

#### `picocalc.fs.read(file, length)`
Reads bytes from an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
  - `length` (number): Number of bytes to read
- **Returns:** (string or nil) Data read, or `nil` on error

```lua
local data = picocalc.fs.read(f, 1024)
```

---

#### `picocalc.fs.write(file, data)`
Writes data to an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
  - `data` (string): Data to write
- **Returns:** (number) Number of bytes written

```lua
local n = picocalc.fs.write(f, "content")
```

---

#### `picocalc.fs.close(file)`
Closes an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
- **Returns:** None

---

#### `picocalc.fs.exists(path)`
Checks if a file or directory exists.

- **Parameters:**
  - `path` (string): Absolute path
- **Returns:** (boolean) `true` if exists, `false` otherwise

```lua
if picocalc.fs.exists("/data/save.txt") then
    -- Load saved data
end
```

---

#### `picocalc.fs.readFile(path)`
Reads an entire file into memory in one call.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (string or nil) File contents, or `nil` on error

```lua
local content = picocalc.fs.readFile("/apps/hello/config.txt")
```

---

#### `picocalc.fs.size(path)`
Returns the size of a file in bytes.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (number) File size in bytes, or 0 on error

```lua
local bytes = picocalc.fs.size("/data/save.txt")
```

---

#### `picocalc.fs.listDir(path)`
Lists the contents of a directory.

- **Parameters:**
  - `path` (string): Absolute directory path
- **Returns:** (table) Array of entries, where each entry is a table with:
  - `name` (string): File or directory name
  - `is_dir` (boolean): `true` if directory, `false` if file
  - `size` (number): File size in bytes (0 for directories)

```lua
local entries = picocalc.fs.listDir("/apps")
for _, e in ipairs(entries) do
    print(e.name, e.is_dir, e.size)
end
```

---

#### `picocalc.fs.mkdir(path)`
Creates a directory at the specified path.

- **Parameters:**
  - `path` (string): Absolute directory path to create
- **Returns:** (boolean) `true` if successful or directory already exists, `false` on error

```lua
-- Create app data directory
local data_dir = "/data/" .. APP_ID
if picocalc.fs.mkdir(data_dir) then
    print("Data directory ready")
end
```

---

## picocalc.wifi

WiFi connectivity functions (Pico 2W only — shares SPI1 with LCD).

### Functions

#### `picocalc.wifi.isAvailable()`
Checks if WiFi hardware is present.

- **Parameters:** None
- **Returns:** (boolean) `true` if WiFi is available (Pico 2W), `false` otherwise

```lua
if picocalc.wifi.isAvailable() then
    picocalc.wifi.connect("MySSID", "password")
end
```

---

#### `picocalc.wifi.connect(ssid [, password])`
Connects to a WiFi network (non-blocking).

- **Parameters:**
  - `ssid` (string): Network SSID
  - `password` (string, optional): Network password (omit for open networks)
- **Returns:** None

```lua
picocalc.wifi.connect("MyWiFi", "password123")
```

---

#### `picocalc.wifi.disconnect()`
Disconnects from the current WiFi network.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.wifi.getStatus()`
Returns the current WiFi connection status.

- **Parameters:** None
- **Returns:** (number) Status code (see constants below)

```lua
if picocalc.wifi.getStatus() == picocalc.wifi.STATUS_CONNECTED then
    print("Connected! IP: " .. picocalc.wifi.getIP())
end
```

---

#### `picocalc.wifi.getIP()`
Returns the current IP address as a string.

- **Parameters:** None
- **Returns:** (string or nil) IP address (e.g., `"192.168.1.100"`), or `nil` if not connected

---

#### `picocalc.wifi.getSSID()`
Returns the SSID of the current connection.

- **Parameters:** None
- **Returns:** (string or nil) SSID, or `nil` if not connected

---

### WiFi Status Constants

| Constant | Description |
|----------|-------------|
| `picocalc.wifi.STATUS_DISCONNECTED` | Not connected |
| `picocalc.wifi.STATUS_CONNECTING` | Connection in progress |
| `picocalc.wifi.STATUS_CONNECTED` | Connected successfully |
| `picocalc.wifi.STATUS_FAILED` | Connection failed |

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

---

## picocalc.perf

Performance monitoring utilities for apps.

### Functions

#### `picocalc.perf.beginFrame()`
Starts timing a frame. Call at the **beginning** of your game loop.

- **Parameters:** None
- **Returns:** None

```lua
while true do
    picocalc.perf.beginFrame()
    -- Game logic
    picocalc.perf.endFrame()
end
```

---

#### `picocalc.perf.endFrame()`
Ends timing a frame and updates FPS calculation. Call at the **end** of your game loop.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.perf.getFPS()`
Returns the current FPS, averaged over the last 30 frames.

- **Parameters:** None
- **Returns:** (number) Frames per second

```lua
local fps = picocalc.perf.getFPS()
```

---

#### `picocalc.perf.getFrameTime()`
Returns the last frame's duration in milliseconds.

- **Parameters:** None
- **Returns:** (number) Milliseconds

```lua
local ms = picocalc.perf.getFrameTime()
```

---

#### `picocalc.perf.drawFPS([x, y])`
Convenience function to draw the FPS counter on screen. Color-coded: green ≥55 FPS, yellow ≥30, red <30.

- **Parameters:**
  - `x` (number, optional): X coordinate. Defaults to 250 (top-right).
  - `y` (number, optional): Y coordinate. Defaults to 8.
- **Returns:** None

```lua
picocalc.perf.drawFPS()  -- Draw at default position
```

---

## Standard Lua Libraries

The following Lua 5.4 standard libraries are available:

| Library | Description |
|---------|-------------|
| `base` | Core functions (`print`, `type`, `pairs`, `ipairs`, `tonumber`, `tostring`, etc.) |
| `table` | Table manipulation (`table.insert`, `table.remove`, `table.sort`, etc.) |
| `string` | String manipulation (`string.sub`, `string.format`, `string.match`, etc.) |
| `math` | Math functions (`math.sin`, `math.random`, `math.floor`, etc.) |
| `utf8` | UTF-8 string support |
| `coroutine` | Coroutine support (`coroutine.create`, `coroutine.resume`, etc.) |

**Not available** (for sandboxing): `io`, `os`, `package`, `debug`

---

## Example: Complete Game Loop

```lua
-- Initialize
local x, y = 160, 160
local color = picocalc.display.WHITE

-- Main loop
while true do
    picocalc.perf.beginFrame()
    picocalc.input.update()
    
    -- Handle input
    local pressed = picocalc.input.getButtonsPressed()
    if pressed & picocalc.input.BTN_UP ~= 0 then y = y - 5 end
    if pressed & picocalc.input.BTN_DOWN ~= 0 then y = y + 5 end
    if pressed & picocalc.input.BTN_ESC ~= 0 then return end
    
    -- Draw
    picocalc.display.clear(picocalc.display.BLACK)
    picocalc.display.fillRect(x - 10, y - 10, 20, 20, color)
    picocalc.perf.drawFPS()
    picocalc.display.flush()
    
    picocalc.perf.endFrame()
    picocalc.sys.sleep(16)  -- ~60 FPS
end
```

---

## Notes

- The display framebuffer is in **PSRAM** — safe to call drawing functions frequently.
- Call `picocalc.input.update()` and `picocalc.display.flush()` **once per frame**.
- The Menu key (F10) is automatically intercepted by the OS to show the system menu overlay.
- All file paths must be absolute (e.g., `"/apps/myapp/data.txt"` or `APP_DIR .. "/data.txt"`).
- WiFi and LCD share **SPI1** — the OS manages arbitration automatically.
- Config data is shared across all apps — use namespaced keys (e.g., `"myapp.highscore"`).

---

**For more examples, see the bundled apps in `/apps/`.**
