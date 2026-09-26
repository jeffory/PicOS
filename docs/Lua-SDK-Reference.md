# Lua SDK Reference

This document provides a complete reference for all Lua APIs available to PicOS apps.

## API Sections

- [[Global Variables and Permissions]]
- [[API Audio and Sound]] — Audio Playback (WAV, MP3) & Tones
- [[API Crypto]] — Cryptographic Primitives
- [[API Display and Graphics]] — Graphics & Display
- [[API Filesystem]] — Filesystem (SD Card)
- [[API Input]] — Keyboard & Button Input
- [[API Modplayer]] — Tracker Module Music (MOD, XM, S3M)
- [[API Network and WiFi]] — WiFi & HTTP Client
- [[API Performance]] — Performance Monitoring
- [[API REPL]] — Interactive Lua REPL
- [[API Sysconfig]] — System-Wide Configuration
- [[API System and Config]] — System Functions & Per-App Config
- [[API Terminal]] — Terminal Emulator Widget
- [[API UI]] — Standard UI Components
- [[API Video]] — MJPEG Video Playback
- [[Standard Lua Libraries]]

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

- The display uses a **double-buffered framebuffer in SRAM** (2× 200 KB). DMA flushes run in the background while the CPU draws the next frame.
- Call `picocalc.input.update()` and `picocalc.display.flush()` **once per frame**.
- The Menu key (F10) is automatically intercepted by the OS to show the system menu overlay.
- All file paths must be absolute (e.g., `"/apps/myapp/data.txt"` or `APP_DIR .. "/data.txt"`).
- Use `picocalc.fs.appPath("filename")` for per-app data storage — it auto-creates the directory.
- Without `root-filesystem` permission, file access is sandboxed to your app's directory and `/data/<appid>/`.
- Per-app config (`picocalc.config`, also available as `picocalc.appconfig`) is isolated per app at `/data/<APP_ID>/config.json`. Only `picocalc.sysconfig` (`/system/config.json`) is shared across all apps.

---

### Native App Development

If you prefer to write apps in C or C++ instead of Lua, see the [[Native App Development]] guide. The C API mirrors the Lua modules documented here.

