# API Input

Keyboard and button input functions.

## picocalc.input

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

#### `picocalc.input.clearState()`
Clear all button and key input state. Resets pressed, released, and held button buffers. Useful when returning from modal dialogs or menus to prevent stale input.

- **Parameters:** None
- **Returns:** None

```lua
-- After closing a dialog, clear stale input
picocalc.input.clearState()
```

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
