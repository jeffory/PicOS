-- ssh_input.lua — Map PicOS keyboard input to SSH/VT100 escape sequences

local input_map = {}

local pc = picocalc
local inp = pc.input

-- Map BTN_* button presses to VT100 escape sequences
-- decckm: when true, arrow keys send ESC O x instead of ESC [ x
function input_map.translate_buttons(buttons, decckm)
    local seqs = {}

    if buttons & inp.BTN_UP ~= 0 then
        seqs[#seqs + 1] = decckm and "\x1bOA" or "\x1b[A"
    end
    if buttons & inp.BTN_DOWN ~= 0 then
        seqs[#seqs + 1] = decckm and "\x1bOB" or "\x1b[B"
    end
    if buttons & inp.BTN_RIGHT ~= 0 then
        seqs[#seqs + 1] = decckm and "\x1bOC" or "\x1b[C"
    end
    if buttons & inp.BTN_LEFT ~= 0 then
        seqs[#seqs + 1] = decckm and "\x1bOD" or "\x1b[D"
    end
    if buttons & inp.BTN_ENTER ~= 0 then
        seqs[#seqs + 1] = "\r"
    end
    if buttons & inp.BTN_TAB ~= 0 then
        seqs[#seqs + 1] = "\t"
    end
    if buttons & inp.BTN_BACKSPACE ~= 0 then
        seqs[#seqs + 1] = "\x7f"  -- DEL (most SSH servers expect this for backspace)
    end
    if buttons & inp.BTN_ESC ~= 0 then
        seqs[#seqs + 1] = "\x1b"
    end

    -- Function keys
    if buttons & inp.BTN_F1 ~= 0 then seqs[#seqs + 1] = "\x1bOP" end
    if buttons & inp.BTN_F2 ~= 0 then seqs[#seqs + 1] = "\x1bOQ" end
    if buttons & inp.BTN_F3 ~= 0 then seqs[#seqs + 1] = "\x1bOR" end
    if buttons & inp.BTN_F4 ~= 0 then seqs[#seqs + 1] = "\x1bOS" end
    if buttons & inp.BTN_F5 ~= 0 then seqs[#seqs + 1] = "\x1b[15~" end

    return table.concat(seqs)
end

-- Translate a character from kbd_get_char() to what SSH expects
function input_map.translate_char(c)
    if not c then return nil end
    local b = string.byte(c)

    -- Control characters (Ctrl+A through Ctrl+Z) pass through as-is
    if b >= 1 and b <= 26 then
        return c
    end

    -- Backspace key sends 0x08, but SSH expects 0x7F (DEL)
    if b == 0x08 then
        return "\x7f"
    end

    -- Enter sends \n, but SSH expects \r
    if b == 0x0A then
        return "\r"
    end

    -- Regular printable characters
    if b >= 0x20 and b < 0x7F then
        return c
    end

    return nil
end

return input_map
