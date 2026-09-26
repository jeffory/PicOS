# API UI

Standard UI components for consistent app appearance. These draw directly to the framebuffer.

## picocalc.ui

### Functions

#### `picocalc.ui.drawHeader(title)`
Draws a standard header bar at the top of the screen with the given title.

- **Parameters:**
  - `title` (string): Title text to display
- **Returns:** None

```lua
picocalc.ui.drawHeader("My App")
```

---

#### `picocalc.ui.drawFooter([leftText [, rightText]])`
Draws a standard footer bar at the bottom of the screen with optional left and right text.

- **Parameters:**
  - `leftText` (string, optional): Text for the left side
  - `rightText` (string, optional): Text for the right side
- **Returns:** None

```lua
picocalc.ui.drawFooter("Press Esc to exit", "Bat: 85%")
```

---

#### `picocalc.ui.drawTabs(y, tabs, activeIndex [, prevKey [, nextKey]])`
Draws a horizontal tab bar and optionally handles navigation key presses.

- **Parameters:**
  - `y` (number): Y coordinate of the top of the tab bar
  - `tabs` (table): Array of tab label strings (e.g., `{"Files", "Settings", "About"}`)
  - `activeIndex` (number): 1-based index of the currently active tab
  - `prevKey` (number, optional): Button bitmask to switch to the previous tab (e.g., `picocalc.input.BTN_LEFT`)
  - `nextKey` (number, optional): Button bitmask to switch to the next tab (e.g., `picocalc.input.BTN_RIGHT`)
- **Returns:** (number, number) `newActiveIndex, heightConsumed` — the (possibly updated) active tab index and the pixel height consumed by the tab bar

```lua
local tabs = {"Files", "Settings", "About"}
local active = 1

while true do
    picocalc.input.update()
    picocalc.display.clear(picocalc.display.BLACK)
    picocalc.ui.drawHeader("My App")
    active, tab_h = picocalc.ui.drawTabs(20, tabs, active,
        picocalc.input.BTN_LEFT, picocalc.input.BTN_RIGHT)
    -- draw content for tabs[active] starting at y=20+tab_h
    picocalc.display.flush()
end
```

---

### Dialogs

#### `picocalc.ui.confirm(message)`
Show a yes/no confirmation dialog. Blocks until user responds.

- **Parameters:**
  - `message` (string): Question text
- **Returns:** (boolean) true if confirmed, false if cancelled

```lua
if picocalc.ui.confirm("Delete this file?") then
    -- user pressed Yes
end
```

---

#### `picocalc.ui.textInput([prompt], [default])`
Show a modal text input dialog. Blocks until user submits or cancels.

- **Parameters:**
  - `prompt` (string, optional): Prompt text
  - `default` (string, optional): Pre-filled text
- **Returns:** (string) entered text, or nil if cancelled

> **Note:** Maximum 128 characters.

```lua
local name = picocalc.ui.textInput("Enter name:", "Player 1")
if name then
    print("Hello, " .. name)
end
```

---

#### `picocalc.ui.splash([status], [subtext])`
Draw a splash/loading screen with the PicOS logo.

- **Parameters:**
  - `status` (string, optional): Status text
  - `subtext` (string, optional): Smaller text below
- **Returns:** None

```lua
picocalc.ui.splash("Loading...", "Please wait")
```

---

### Widget Drawing Primitives

#### `picocalc.ui.drawPanel(x, y, w, h, [title])`
Draw a panel/card container with optional title bar.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `h` (number): Height
  - `title` (string, optional): Panel title
- **Returns:** None

```lua
picocalc.ui.drawPanel(10, 30, 300, 200, "Settings")
```

---

#### `picocalc.ui.drawButton(x, y, w, label, focused, [pressed])`
Draw a button widget.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `label` (string): Button label text
  - `focused` (boolean): Whether the button has focus
  - `pressed` (boolean, optional): Whether the button is pressed
- **Returns:** None

```lua
picocalc.ui.drawButton(10, 100, 80, "OK", true, false)
```

---

#### `picocalc.ui.drawTextField(x, y, w, text, cursorPos, scrollOffset, focused, showCursor)`
Draw a single-line text input field. This is a rendering primitive — it does NOT handle input.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `text` (string): Current text content
  - `cursorPos` (number): Cursor character position
  - `scrollOffset` (number): Horizontal scroll offset
  - `focused` (boolean): Whether the field has focus
  - `showCursor` (boolean): Whether to display the cursor
- **Returns:** None

```lua
picocalc.ui.drawTextField(10, 50, 200, "Hello", 5, 0, true, true)
```

---

#### `picocalc.ui.drawTextArea(x, y, w, h, text, segments, scrollY, cursorRow, cursorCol, focused, showCursor)`
Draw a multi-line text area widget. This is a rendering primitive.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `h` (number): Height
  - `text` (string): Full text content
  - `segments` (table): Array of line-start byte offsets (from `wrapText`)
  - `scrollY` (number): Vertical scroll offset in rows
  - `cursorRow` (number): Cursor row
  - `cursorCol` (number): Cursor column
  - `focused` (boolean): Whether the area has focus
  - `showCursor` (boolean): Whether to display the cursor
- **Returns:** None

```lua
local segments, rows = picocalc.ui.wrapText(myText, 40)
picocalc.ui.drawTextArea(10, 30, 250, 200, myText, segments, 0, 0, 0, true, true)
```

---

#### `picocalc.ui.drawListItem(x, y, w, text, selected, focused)`
Draw a selectable list item.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `text` (string): Item text
  - `selected` (boolean): Whether the item is selected
  - `focused` (boolean): Whether the item has focus
- **Returns:** None

```lua
for i, item in ipairs(items) do
    picocalc.ui.drawListItem(10, 30 + (i-1) * 20, 300, item, i == selectedIdx, true)
end
```

---

#### `picocalc.ui.drawProgress(x, y, w, h, progress, [fillColor])`
Draw a progress bar.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `h` (number): Height
  - `progress` (number): Progress value from 0.0 to 1.0
  - `fillColor` (number, optional): RGB565 color (defaults to green)
- **Returns:** None

```lua
picocalc.ui.drawProgress(10, 100, 200, 16, 0.75)
```

---

#### `picocalc.ui.drawCheckbox(x, y, checked, focused)`
Draw a checkbox widget.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `checked` (boolean): Whether the checkbox is checked
  - `focused` (boolean): Whether the checkbox has focus
- **Returns:** None

```lua
picocalc.ui.drawCheckbox(10, 50, true, false)
```

---

#### `picocalc.ui.drawRadio(x, y, selected, focused)`
Draw a radio button widget.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `selected` (boolean): Whether the radio button is selected
  - `focused` (boolean): Whether the radio button has focus
- **Returns:** None

```lua
picocalc.ui.drawRadio(10, 50, true, false)
```

---

#### `picocalc.ui.drawDivider(x, y, w, [color])`
Draw a horizontal divider line.

- **Parameters:**
  - `x` (number): X position
  - `y` (number): Y position
  - `w` (number): Width
  - `color` (number, optional): RGB565 color
- **Returns:** None

```lua
picocalc.ui.drawDivider(10, 80, 300)
```

---

#### `picocalc.ui.drawSpinner(cx, cy, [radius], [frame])`
Draw an animated loading spinner.

- **Parameters:**
  - `cx` (number): Center X position
  - `cy` (number): Center Y position
  - `radius` (number, optional): Spinner radius (default 8)
  - `frame` (number, optional): Animation frame counter (default 0)
- **Returns:** None

```lua
local frame = 0
-- in render loop:
frame = frame + 1
picocalc.ui.drawSpinner(160, 160, 12, frame)
```

---

#### `picocalc.ui.drawToast(y, text, [bgColor])`
Draw a toast notification bar at the given Y position.

- **Parameters:**
  - `y` (number): Y position
  - `text` (string): Toast message text
  - `bgColor` (number, optional): RGB565 background color
- **Returns:** None

```lua
picocalc.ui.drawToast(280, "File saved!")
```

---

#### `picocalc.ui.toast(text, [style])`
Push a system toast notification that auto-displays and auto-dismisses.

- **Parameters:**
  - `text` (string): Toast message text
  - `style` (number, optional): One of the `TOAST_*` constants (default `TOAST_INFO`)
- **Returns:** None

```lua
picocalc.ui.toast("Download complete", picocalc.ui.TOAST_INFO)
```

---

#### `picocalc.ui.wrapText(text, maxCols)`
Word-wrap text to fit within a column width. Returns segment offsets for use with `drawTextArea`.

- **Parameters:**
  - `text` (string): Text to wrap
  - `maxCols` (number): Maximum columns per line
- **Returns:** (table, number) segments array of byte offsets, number of wrapped rows

> **Note:** Maximum 512 segments.

```lua
local segments, rows = picocalc.ui.wrapText(longText, 40)
picocalc.ui.drawTextArea(10, 30, 250, 200, longText, segments, 0, 0, 0, true, true)
```
