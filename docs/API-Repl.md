---
title: "API REPL"
---

Interactive read-eval-print loop primitives for building console-style applications. The REPL module manages a scrolling text display with command-line input and history.

## picocalc.repl

### Functions

#### `picocalc.repl.readline()`
Non-blocking read of a single input event. Call this in a loop to build an interactive prompt. Handles keyboard input, scrolling (up/down arrows), backspace, and system menu detection internally.

- **Parameters:** None
- **Returns:** (string or nil) The input line when Enter is pressed, or `nil` on Esc or when no complete line is ready yet

```lua
while true do
    local line = picocalc.repl.readline()
    if line then
        picocalc.repl.print("> " .. line)
        -- process the command
    end
end
```

---

#### `picocalc.repl.print(...)`
Print values to the REPL output area. Accepts multiple arguments which are converted to strings and separated by tabs. Supports nil, boolean, number, string, table, function, and userdata types.

Values go through `tostring()` (so `__tostring` metamethods are honoured). The output keeps a 256-line scrollback.

- **Parameters:**
  - `...` (any): One or more values to print
- **Returns:** None

```lua
picocalc.repl.print("Hello", 42, true)
-- Output: Hello	42	true
```

---

#### `picocalc.repl.clear()`
Clear all text from the REPL display and reset the scroll position.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.repl.clear()
picocalc.repl.print("Screen cleared.")
```

---

#### `picocalc.repl.echo(enabled)`
Enable or disable input echo. When disabled, typed characters are not displayed on screen. Useful for password prompts or hidden input.

- **Parameters:**
  - `enabled` (boolean): `true` to show typed characters, `false` to hide them
- **Returns:** None

```lua
picocalc.repl.echo(false)
picocalc.repl.print("Enter password:")
local password = nil
while not password do
    password = picocalc.repl.readline()
end
picocalc.repl.echo(true)
```
