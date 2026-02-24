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
