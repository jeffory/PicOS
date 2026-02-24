# API Display and Graphics

Graphics and display functions. The display is **320×320 pixels** with RGB565 color format.

## picocalc.display

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

## picocalc.graphics

Image loading, drawing, and state management. Images are stored in PSRAM and support BMP, JPEG, PNG, and GIF formats.

### State Functions

#### `picocalc.graphics.setColor(color)`
Sets the current drawing color for graphics operations.

- **Parameters:**
  - `color` (number): RGB565 color value
- **Returns:** None

---

#### `picocalc.graphics.setBackgroundColor(color)`
Sets the background color for graphics operations.

- **Parameters:**
  - `color` (number): RGB565 color value
- **Returns:** None

---

#### `picocalc.graphics.clear([color])`
Clears the screen using the background color (or a specified color).

- **Parameters:**
  - `color` (number, optional): RGB565 color value. Defaults to the current background color.
- **Returns:** None

```lua
picocalc.graphics.setBackgroundColor(picocalc.display.BLACK)
picocalc.graphics.clear()
```

---

### Compound Drawing Functions

#### `picocalc.graphics.drawGrid(x, y, cell_w, cell_h, cols, rows, color)`
Draws a grid of `cols×rows` outlined cells in a single C call.

- **Parameters:**
  - `x`, `y` (number): Top-left corner of the grid
  - `cell_w`, `cell_h` (number): Width and height of each cell in pixels
  - `cols`, `rows` (number): Number of columns and rows
  - `color` (number): RGB565 border color
- **Returns:** None

```lua
picocalc.graphics.drawGrid(10, 10, 14, 14, 10, 20, picocalc.display.GRAY)
```

---

#### `picocalc.graphics.fillBorderedRect(x, y, w, h, fill_color, border_color)`
Fills a rectangle then draws a 1-pixel border over it in a single C call.

- **Parameters:**
  - `x`, `y` (number): Top-left corner
  - `w`, `h` (number): Width and height in pixels
  - `fill_color` (number): RGB565 fill color
  - `border_color` (number): RGB565 border color
- **Returns:** None

```lua
picocalc.graphics.fillBorderedRect(10, 10, 50, 50, picocalc.display.BLUE, picocalc.display.WHITE)
```

---

#### `picocalc.graphics.updateDrawParticles(flat_array, delta_s)`
Updates, draws, and compacts a flat particle array in a single C call. The array holds 6 values per particle: `x, y, vx, vy, life_ms, color`.

- **Parameters:**
  - `flat_array` (table): Flat sequence with 6 values per particle
  - `delta_s` (number): Elapsed time in seconds since last call
- **Returns:** (number) Count of live particles remaining

```lua
-- particles = {x, y, vx, vy, life_ms, color, ...}
local live = picocalc.graphics.updateDrawParticles(particles, delta / 1000)
```

---

#### `picocalc.graphics.draw3DWireframe(verts, edges, aX, aY, aZ, scx, scy, fov, edgeColor [, vertColor [, vertSize]])`
Rotates, projects, and draws a 3D wireframe model in a single C call. All trigonometry and matrix math runs in C — suitable for real-time use in game loops.

- **Parameters:**
  - `verts` (table): Flat sequence `{x1, y1, z1, x2, y2, z2, ...}` — `n/3` vertices
  - `edges` (table): Flat sequence `{a1, b1, a2, b2, ...}` — 1-based vertex index pairs
  - `aX`, `aY`, `aZ` (number): Rotation angles in radians, applied in X→Y→Z order
  - `scx`, `scy` (number): Screen-space center point (projection origin)
  - `fov` (number): Field-of-view scale factor (larger = more perspective, try 200–400)
  - `edgeColor` (number): RGB565 color for edges
  - `vertColor` (number, optional): RGB565 color for vertex dots. Defaults to `edgeColor`.
  - `vertSize` (number, optional): Dot size in pixels for vertex dots. Defaults to 3.
- **Returns:** None

```lua
local verts = {
    -1,-1,-1,  1,-1,-1,  1,1,-1, -1,1,-1,  -- back face
    -1,-1, 1,  1,-1, 1,  1,1, 1, -1,1, 1,  -- front face
}
local edges = {
    1,2, 2,3, 3,4, 4,1,  -- back
    5,6, 6,7, 7,8, 8,5,  -- front
    1,5, 2,6, 3,7, 4,8,  -- sides
}
local angle = 0
while true do
    angle = angle + 0.02
    picocalc.display.clear(picocalc.display.BLACK)
    picocalc.graphics.draw3DWireframe(verts, edges, angle, angle*0.7, 0,
        160, 160, 300, picocalc.display.WHITE)
    picocalc.display.flush()
end
```

---

### Image Constructor Functions

#### `picocalc.graphics.image.new(width, height)`
Creates a new blank image in PSRAM, initialized to all zeros (black).

- **Parameters:**
  - `width` (number): Image width in pixels
  - `height` (number): Image height in pixels
- **Returns:** (userdata) Image object
- **Errors:** If dimensions are invalid or memory allocation fails

```lua
local canvas = picocalc.graphics.image.new(64, 64)
```

---

#### `picocalc.graphics.image.load(path)`
Loads an image from the SD card. Supports **BMP**, **JPEG**, **PNG**, and **GIF** (first frame only) formats.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (userdata) Image object
- **Errors:** If file not found, format unsupported, or memory allocation fails

```lua
local img = picocalc.graphics.image.load("/apps/myapp/sprite.bmp")
```

---

#### `picocalc.graphics.image.loadFromBuffer(data)`
Decodes an image from an in-memory buffer. Supports JPEG, PNG, and GIF.

- **Parameters:**
  - `data` (string or userdata): Image file data
- **Returns:** (userdata) Image object
- **Errors:** If format unsupported or decoding fails

```lua
local raw = picocalc.fs.readFile("/apps/myapp/photo.jpg")
local img = picocalc.graphics.image.loadFromBuffer(raw)
```

---

#### `picocalc.graphics.image.getSupportedFormats()`
Returns a table of supported image format names.

- **Returns:** (table) Array of format strings (e.g., `{"BMP", "JPEG", "PNG", "GIF"}`)

---

### Image Methods

All methods are called on image objects with colon syntax.

#### `img:getSize()`
Returns the image dimensions.

- **Returns:** (number, number) `width, height`

```lua
local w, h = img:getSize()
```

---

#### `img:draw(x, y [, flipOpts [, srcRect]])`
Draws the image (or a sub-rectangle of it) to the framebuffer.

- **Parameters:**
  - `x` (number): Destination X coordinate
  - `y` (number): Destination Y coordinate
  - `flipOpts` (boolean or table, optional): If `true`, flips horizontally. If table: `{flipX=bool, flipY=bool}`
  - `srcRect` (table, optional): Source sub-rectangle `{x=int, y=int, w=int, h=int}`
- **Returns:** None

```lua
-- Draw full image
img:draw(10, 20)

-- Draw horizontally flipped
img:draw(10, 20, true)

-- Draw with flip options
img:draw(10, 20, {flipX = true, flipY = false})

-- Draw a sub-region
img:draw(10, 20, false, {x = 0, y = 0, w = 32, h = 32})
```

---

#### `img:drawAnchored(x, y, anchorX, anchorY)`
Draws the image positioned relative to an anchor point.

- **Parameters:**
  - `x` (number): Anchor X coordinate
  - `y` (number): Anchor Y coordinate
  - `anchorX` (number): Horizontal anchor (0.0 = left, 0.5 = center, 1.0 = right)
  - `anchorY` (number): Vertical anchor (0.0 = top, 0.5 = center, 1.0 = bottom)
- **Returns:** None

```lua
-- Draw centered on screen
img:drawAnchored(160, 160, 0.5, 0.5)
```

---

#### `img:drawTiled(x, y, width, height)`
Tiles the image to fill a rectangular area.

- **Parameters:**
  - `x` (number): Top-left X coordinate
  - `y` (number): Top-left Y coordinate
  - `width` (number): Fill area width
  - `height` (number): Fill area height
- **Returns:** None

```lua
-- Tile a pattern across a 200x100 area
img:drawTiled(0, 30, 200, 100)
```

---

#### `img:drawScaled(x, y, scale [, angle])`
Draws the image scaled and optionally rotated.

- **Parameters:**
  - `x` (number): Destination X coordinate
  - `y` (number): Destination Y coordinate
  - `scale` (number): Scale factor (1.0 = original size, 2.0 = double)
  - `angle` (number, optional): Rotation angle in radians. Defaults to 0.
- **Returns:** None

```lua
img:drawScaled(160, 160, 2.0)        -- 2x zoom
img:drawScaled(160, 160, 1.0, 0.785) -- Rotate 45°
```

---

#### `img:copy()`
Creates a deep copy of the image.

- **Returns:** (userdata) New image object with identical pixel data

```lua
local backup = img:copy()
```
