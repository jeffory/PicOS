-- editor.lua — Code editor for PicoForge
-- Wires Buffer + Tokenizer + Terminal SDK together

local Editor = {}
Editor.__index = Editor

-- Syntax highlight colors (RGB565)
local COLORS = {
    keyword  = 0x653F,  -- bright blue
    string   = 0x0F80,  -- green
    comment  = 0x7BEF,  -- gray
    number   = 0xFD60,  -- orange
    funcname = 0xFEE0,  -- yellow
    api      = 0x07FF,  -- cyan
    default  = 0xD6BA,  -- light gray/white
}

local BG_COLOR = 0x0000  -- black background

function Editor.new(term, Buffer, Tokenizer)
    local cols = term:getCols()  -- available columns
    local rows = term:getRows()  -- available rows

    return setmetatable({
        term = term,
        Buffer = Buffer,
        Tokenizer = Tokenizer,
        buffer = nil,       -- active buffer
        visible_rows = rows,
        visible_cols = cols,
        dirty_from = 0,     -- line to re-render from (0 = all)
        line_states = {},    -- per-line tokenizer entry states
        find_active = false,
        find_needle = "",
        find_matches = {},   -- {{line, col}, ...}
        find_idx = 0,
        replace_active = false,
        replace_text = "",
        autocomplete_active = false,
        autocomplete_list = {},
        autocomplete_idx = 0,
        autocomplete_prefix = "",
    }, Editor)
end

function Editor:set_buffer(buf)
    self.buffer = buf
    self.line_states = {}
    self.dirty_from = 0
end

------------------------------------------------------------
-- Tokenizer state management
------------------------------------------------------------

-- Re-tokenize from line_from to end or until states converge
function Editor:retokenize(line_from)
    if not self.buffer then return end
    local total = self.buffer:total_lines()
    local prev_state = (line_from > 1) and self.line_states[line_from - 1] or nil

    for y = line_from, total do
        local line = self.buffer:get_line(y)
        local _, exit_state = self.Tokenizer.tokenize_line(line, prev_state)
        local old_state = self.line_states[y]
        self.line_states[y] = exit_state
        prev_state = exit_state
        -- If state converged with what we had, stop
        if y > line_from and old_state == exit_state then
            break
        end
    end
end

------------------------------------------------------------
-- Rendering
------------------------------------------------------------

function Editor:render()
    if not self.buffer then return end
    local buf = self.buffer
    local term = self.term

    -- Update scroll
    buf:update_scroll(self.visible_rows, self.visible_cols)

    -- Line numbers
    term:setLineNumberStart(buf.scroll_y + 1)

    -- Scrollbar
    term:setScrollInfo(buf:total_lines(), buf.scroll_y)

    -- Clear and write visible lines
    term:clear()
    for row = 0, self.visible_rows - 1 do
        local line_idx = buf.scroll_y + row + 1
        if line_idx > buf:total_lines() then break end
        local line = buf:get_line(line_idx)

        -- Apply horizontal scroll
        local visible = ""
        if buf.scroll_x > 0 then
            visible = line:sub(buf.scroll_x + 1)
        else
            visible = line
        end
        -- Truncate to visible width
        if #visible > self.visible_cols then
            visible = visible:sub(1, self.visible_cols)
        end

        -- Write the line text
        term:setCursor(0, row)
        if #visible > 0 then
            term:write(visible)
        end
    end

    -- Apply syntax highlighting
    self:apply_highlighting()

    -- Position cursor
    local cx = buf.cursor_x - 1 - buf.scroll_x
    local cy = buf.cursor_y - 1 - buf.scroll_y
    term:setCursor(cx, cy)

    term:render()
end

function Editor:render_dirty()
    if not self.buffer then return end
    local buf = self.buffer

    buf:update_scroll(self.visible_rows, self.visible_cols)
    term:setLineNumberStart(buf.scroll_y + 1)
    term:setScrollInfo(buf:total_lines(), buf.scroll_y)

    -- Only re-render changed lines
    local from = self.dirty_from
    if from > 0 then
        for row = 0, self.visible_rows - 1 do
            local line_idx = buf.scroll_y + row + 1
            if line_idx >= from then
                if line_idx > buf:total_lines() then
                    -- Clear leftover lines
                    term:setCursor(0, row)
                    term:write(string.rep(" ", self.visible_cols))
                else
                    local line = buf:get_line(line_idx)
                    local visible = line
                    if buf.scroll_x > 0 then visible = line:sub(buf.scroll_x + 1) end
                    if #visible > self.visible_cols then visible = visible:sub(1, self.visible_cols) end
                    term:setCursor(0, row)
                    term:write(visible .. string.rep(" ", self.visible_cols - #visible))
                end
            end
        end
        self:apply_highlighting()
    end

    local cx = buf.cursor_x - 1 - buf.scroll_x
    local cy = buf.cursor_y - 1 - buf.scroll_y
    term:setCursor(cx, cy)

    self.dirty_from = 0
    term:renderDirty()
end

function Editor:apply_highlighting()
    if not self.buffer then return end
    local buf = self.buffer

    for row = 0, self.visible_rows - 1 do
        local line_idx = buf.scroll_y + row + 1
        if line_idx > buf:total_lines() then break end

        local line = buf:get_line(line_idx)
        local entry_state = (line_idx > 1) and self.line_states[line_idx - 1] or nil
        local tokens, _ = self.Tokenizer.tokenize_line(line, entry_state)

        -- Build color arrays for this row
        local fg_arr = {}
        local bg_arr = {}
        for col = 1, self.visible_cols do
            fg_arr[col] = COLORS.default
            bg_arr[col] = BG_COLOR
        end

        for _, tok in ipairs(tokens) do
            local color = COLORS[tok[3]] or COLORS.default
            -- Adjust for horizontal scroll
            local start_col = tok[1] - buf.scroll_x
            local end_col = tok[2] - buf.scroll_x
            if end_col >= 1 and start_col <= self.visible_cols then
                if start_col < 1 then start_col = 1 end
                if end_col > self.visible_cols then end_col = self.visible_cols end
                for c = start_col, end_col do
                    fg_arr[c] = color
                end
            end
        end

        -- Highlight find matches on this line
        if self.find_active and #self.find_needle > 0 then
            for _, m in ipairs(self.find_matches) do
                if m[1] == line_idx then
                    local ms = m[2] - buf.scroll_x
                    local me = ms + #self.find_needle - 1
                    if me >= 1 and ms <= self.visible_cols then
                        if ms < 1 then ms = 1 end
                        if me > self.visible_cols then me = self.visible_cols end
                        for c = ms, me do
                            bg_arr[c] = 0xFFE0  -- yellow highlight
                            fg_arr[c] = 0x0000  -- black text
                        end
                    end
                end
            end
        end

        self.term:setRowColors(row, fg_arr, bg_arr, 0, self.visible_cols)
    end
end

------------------------------------------------------------
-- Key handling
------------------------------------------------------------

function Editor:handle_key(key)
    if not self.buffer then return false end
    local buf = self.buffer

    -- Autocomplete handling
    if self.autocomplete_active then
        if key == "up" then
            self.autocomplete_idx = ((self.autocomplete_idx - 2) % #self.autocomplete_list) + 1
            return true
        elseif key == "down" then
            self.autocomplete_idx = (self.autocomplete_idx % #self.autocomplete_list) + 1
            return true
        elseif key == "return" or key == "tab" then
            self:accept_autocomplete()
            return true
        elseif key == "escape" then
            self.autocomplete_active = false
            return true
        end
        -- Fall through for other keys — close autocomplete
        self.autocomplete_active = false
    end

    -- Find mode
    if self.find_active then
        return self:handle_find_key(key)
    end

    -- Normal editing keys
    if key == "up" then
        buf:move_cursor(0, -1)
    elseif key == "down" then
        buf:move_cursor(0, 1)
    elseif key == "left" then
        buf:move_cursor(-1, 0)
    elseif key == "right" then
        buf:move_cursor(1, 0)
    elseif key == "home" then
        buf.cursor_x = 1
        buf:clamp_cursor()
    elseif key == "end" then
        buf.cursor_x = #buf:get_line(buf.cursor_y) + 1
    elseif key == "pageUp" then
        buf:move_cursor(0, -self.visible_rows)
    elseif key == "pageDown" then
        buf:move_cursor(0, self.visible_rows)
    elseif key == "backspace" then
        buf:backspace()
        self.dirty_from = buf.cursor_y
        self:retokenize(buf.cursor_y)
    elseif key == "delete" then
        buf:delete_char()
        self.dirty_from = buf.cursor_y
        self:retokenize(buf.cursor_y)
    elseif key == "return" then
        local old_y = buf.cursor_y
        buf:insert_newline_auto(true, 4)
        self.dirty_from = old_y
        self:retokenize(old_y)
    elseif key == "tab" then
        buf:insert_tab(true, 4)
        self.dirty_from = buf.cursor_y
        self:retokenize(buf.cursor_y)
    else
        return false
    end

    return true
end

function Editor:handle_char(ch)
    if not self.buffer then return false end
    if not ch or #ch == 0 then return false end
    local buf = self.buffer
    buf:insert_char(ch)
    self.dirty_from = buf.cursor_y
    self:retokenize(buf.cursor_y)
    return true
end

-- Ctrl shortcuts
function Editor:handle_ctrl(key)
    if not self.buffer then return false end
    local buf = self.buffer

    if key == "z" then
        buf:undo()
        self.dirty_from = 0  -- full re-render
        self:retokenize(1)
        return true
    elseif key == "y" then
        buf:redo()
        self.dirty_from = 0
        self:retokenize(1)
        return true
    elseif key == "k" then
        buf:delete_line()
        self.dirty_from = buf.cursor_y
        self:retokenize(buf.cursor_y)
        return true
    elseif key == "f" then
        self:start_find()
        return true
    elseif key == "h" then
        self:start_replace()
        return true
    elseif key == " " then
        self:start_autocomplete()
        return true
    end

    return false
end

------------------------------------------------------------
-- Find / Replace
------------------------------------------------------------

function Editor:start_find()
    self.find_active = true
    self.replace_active = false
    self.find_needle = ""
    self.find_matches = {}
    self.find_idx = 0
end

function Editor:start_replace()
    self.find_active = true
    self.replace_active = true
    self.find_needle = ""
    self.replace_text = ""
    self.find_matches = {}
    self.find_idx = 0
end

function Editor:update_find_matches()
    self.find_matches = {}
    if #self.find_needle == 0 then return end
    local buf = self.buffer
    for y = 1, buf:total_lines() do
        local line = buf:get_line(y)
        local pos = 1
        while true do
            local x = line:find(self.find_needle, pos, true)
            if not x then break end
            self.find_matches[#self.find_matches + 1] = {y, x}
            pos = x + 1
        end
    end
end

function Editor:goto_next_match()
    if #self.find_matches == 0 then return end
    self.find_idx = (self.find_idx % #self.find_matches) + 1
    local m = self.find_matches[self.find_idx]
    self.buffer.cursor_y = m[1]
    self.buffer.cursor_x = m[2]
    self.buffer:clamp_cursor()
end

function Editor:goto_prev_match()
    if #self.find_matches == 0 then return end
    self.find_idx = ((self.find_idx - 2) % #self.find_matches) + 1
    local m = self.find_matches[self.find_idx]
    self.buffer.cursor_y = m[1]
    self.buffer.cursor_x = m[2]
    self.buffer:clamp_cursor()
end

function Editor:handle_find_key(key)
    if key == "escape" then
        self.find_active = false
        self.replace_active = false
        self.find_matches = {}
        return true
    elseif key == "return" or key == "down" then
        self:goto_next_match()
        return true
    elseif key == "up" then
        self:goto_prev_match()
        return true
    elseif key == "backspace" then
        if #self.find_needle > 0 then
            self.find_needle = self.find_needle:sub(1, -2)
            self:update_find_matches()
        end
        return true
    end
    return false
end

function Editor:handle_find_char(ch)
    if not self.find_active then return false end
    self.find_needle = self.find_needle .. ch
    self:update_find_matches()
    self:goto_next_match()
    return true
end

function Editor:replace_current()
    if #self.find_matches == 0 or self.find_idx == 0 then return end
    local m = self.find_matches[self.find_idx]
    self.buffer:replace_at(m[1], m[2], self.find_needle, self.replace_text)
    self:update_find_matches()
    self.dirty_from = 0
    self:retokenize(1)
end

function Editor:replace_all()
    -- Replace backwards to preserve positions
    for i = #self.find_matches, 1, -1 do
        local m = self.find_matches[i]
        self.buffer:replace_at(m[1], m[2], self.find_needle, self.replace_text)
    end
    self:update_find_matches()
    self.dirty_from = 0
    self:retokenize(1)
end

------------------------------------------------------------
-- Autocomplete
------------------------------------------------------------

function Editor:start_autocomplete()
    if not self.buffer then return end
    -- Get word before cursor
    local line = self.buffer:get_line(self.buffer.cursor_y)
    local before = line:sub(1, self.buffer.cursor_x - 1)
    local prefix = before:match("([%a_][%w_]*)$") or ""
    if #prefix == 0 then return end

    self.autocomplete_prefix = prefix
    -- Gather function names from buffer
    local funcs = self.buffer:scan_functions()

    -- Filter by prefix
    self.autocomplete_list = {}
    for _, name in ipairs(funcs) do
        if name:sub(1, #prefix) == prefix and name ~= prefix then
            self.autocomplete_list[#self.autocomplete_list + 1] = name
        end
    end

    if #self.autocomplete_list > 0 then
        self.autocomplete_active = true
        self.autocomplete_idx = 1
    end
end

function Editor:accept_autocomplete()
    if not self.autocomplete_active or #self.autocomplete_list == 0 then return end
    local completion = self.autocomplete_list[self.autocomplete_idx]
    local suffix = completion:sub(#self.autocomplete_prefix + 1)
    for i = 1, #suffix do
        self.buffer:insert_char(suffix:sub(i, i))
    end
    self.autocomplete_active = false
    self.dirty_from = self.buffer.cursor_y
    self:retokenize(self.buffer.cursor_y)
end

-- Draw autocomplete popup (called from main render)
function Editor:draw_autocomplete(disp)
    if not self.autocomplete_active or #self.autocomplete_list == 0 then return end

    local buf = self.buffer
    -- Position popup below cursor
    local cx = (buf.cursor_x - 1 - buf.scroll_x) * 6 + 24  -- account for line numbers
    local cy = (buf.cursor_y - buf.scroll_y) * 11 + 40 + 11  -- below cursor line

    local max_show = math.min(#self.autocomplete_list, 8)
    local popup_w = 0
    for i = 1, max_show do
        local w = #self.autocomplete_list[i] * 6 + 8
        if w > popup_w then popup_w = w end
    end
    local popup_h = max_show * 11 + 4

    -- Clamp to screen
    if cx + popup_w > 316 then cx = 316 - popup_w end
    if cy + popup_h > 300 then cy = cy - popup_h - 11 end

    -- Background
    disp.fillRect(cx, cy, popup_w, popup_h, 0x2104)
    disp.drawRect(cx, cy, popup_w, popup_h, 0x4208)

    -- Items
    for i = 1, max_show do
        local y = cy + 2 + (i - 1) * 11
        local fg = 0xD6BA
        if i == self.autocomplete_idx then
            disp.fillRect(cx + 1, y, popup_w - 2, 11, 0x4A69)
            fg = 0xFFFF
        end
        disp.drawText(cx + 4, y + 1, self.autocomplete_list[i], fg)
    end
end

return Editor
