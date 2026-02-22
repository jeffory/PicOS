-- keytest: display every key event for debugging the STM32 keyboard controller.
-- Shows raw hex keycodes so unknown keys (F1-F10, Sym combos, etc.) can be identified.

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local sys   = pc.sys

local BG  = disp.BLACK
local FG  = disp.WHITE
local DIM = disp.GRAY
local ACC = disp.CYAN
local YEL = disp.YELLOW

local BTN_LABELS = {
    { input.BTN_UP,    "UP"    },
    { input.BTN_DOWN,  "DOWN"  },
    { input.BTN_LEFT,  "LEFT"  },
    { input.BTN_RIGHT, "RIGHT" },
    { input.BTN_ENTER, "Enter" },
    { input.BTN_ESC,   "Esc"   },
    { input.BTN_MENU,  "Sym"   },
}

local function btn_names(mask)
    if mask == 0 then return nil end
    local t = {}
    for _, b in ipairs(BTN_LABELS) do
        if mask & b[1] ~= 0 then t[#t + 1] = b[2] end
    end
    return #t > 0 and table.concat(t, "+") or nil
end

-- Rolling event log, newest first
local MAX_HISTORY = 16
local history = {}

local function push(line)
    table.insert(history, 1, line)
    if #history > MAX_HISTORY then table.remove(history) end
end

-- ── Main loop ──────────────────────────────────────────────────────────────────

while true do
    input.update()   -- one poll per frame

    local raw      = input.getRawKey()        -- raw keycode this frame (0 = none)
    local pressed  = input.getButtonsPressed()
    local released = input.getButtonsReleased()
    local ch       = input.getChar()
    
    if input.getButtons() & input.BTN_ESC ~= 0 then return end

    -- Log events
    if raw ~= 0 then
        local label = ""
        if ch then
            if string.byte(ch) == 8 then
                label = "<Backspace>"
            else
                label = string.format("'%s'", ch)
            end
        elseif btn_names(pressed) then
            label = btn_names(pressed)
        end
        push(string.format("0x%02X  %s", raw, label))
    end
    if released ~= 0 and not (released & input.BTN_ESC ~= 0) then
        local name = btn_names(released)
        if name then push(string.format("      release: %s", name)) end
    end

    -- ── Draw ──────────────────────────────────────────────────────────────────
    disp.clear(BG)

    -- Header
    pc.ui.drawHeader("Key Test")
    
    pc.ui.drawFooter("Esc = exit", nil)

    -- Last raw keycode (large, prominent)
    disp.drawText(8, 36, "Last key:", DIM, BG)
    if raw ~= 0 then
        disp.drawText(72, 36, string.format("0x%02X  (%d)", raw, raw), YEL, BG)
    else
        disp.drawText(72, 28, "--", DIM, BG)
    end

    -- Currently held buttons
    local held_str = btn_names(input.getButtons()) or "(none)"
    disp.drawText(8, 50, "Held:", DIM, BG)
    disp.drawText(56, 50, held_str, FG, BG)

    -- Divider + log
    disp.fillRect(0, 64, 320, 1, disp.rgb(60, 60, 100))
    disp.drawText(8, 68, "Event log (hex = raw STM32 keycode):", DIM, BG)

    if #history == 0 then
        disp.drawText(8, 82, "Press any key...", DIM, BG)
    else
        for i, entry in ipairs(history) do
            local y = 82 + (i - 1) * 14
            if y > 308 then break end
            disp.drawText(8, y, entry, i == 1 and FG or DIM, BG)
        end
    end

    disp.flush()
    sys.sleep(16)
end
