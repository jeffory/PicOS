# API Performance

Performance monitoring utilities for apps.

## picocalc.perf

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
