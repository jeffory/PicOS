-- buffer.lua — Text buffer with undo/redo for PicoForge
-- Pure Lua module, no picocalc dependencies (unit-testable on host)

local Buffer = {}
Buffer.__index = Buffer

local MAX_UNDO = 200
local COALESCE_MAX = 50

function Buffer.new(filepath)
    return setmetatable({
        lines = {""},
        cursor_x = 1,       -- column, 1-indexed
        cursor_y = 1,       -- line, 1-indexed
        scroll_y = 0,       -- top visible line, 0-indexed
        scroll_x = 0,       -- horizontal scroll offset, 0-indexed
        filepath = filepath,
        modified = false,
        undo_stack = {},
        redo_stack = {},
        _coalesce = nil,    -- current coalescing entry
    }, Buffer)
end

-- Load content from a string (for testing or when fs provides content)
function Buffer:load_string(content)
    self.lines = {}
    if not content or content == "" then
        self.lines[1] = ""
    else
        -- Split by newlines, handle \r\n and \n
        local pos = 1
        while pos <= #content do
            local nl = content:find("\n", pos, true)
            if nl then
                local line = content:sub(pos, nl - 1)
                -- Strip trailing \r
                if line:sub(-1) == "\r" then
                    line = line:sub(1, -2)
                end
                self.lines[#self.lines + 1] = line
                pos = nl + 1
            else
                self.lines[#self.lines + 1] = content:sub(pos)
                pos = #content + 1
            end
        end
        -- If content ends with \n, add empty trailing line
        if content:sub(-1) == "\n" then
            self.lines[#self.lines + 1] = ""
        end
    end
    self.cursor_x = 1
    self.cursor_y = 1
    self.scroll_y = 0
    self.scroll_x = 0
    self.modified = false
    self.undo_stack = {}
    self.redo_stack = {}
    self._coalesce = nil
end

-- Serialize buffer content to string
function Buffer:to_string()
    return table.concat(self.lines, "\n") .. "\n"
end

-- Total line count
function Buffer:total_lines()
    return #self.lines
end

-- Get line content (1-indexed)
function Buffer:get_line(idx)
    return self.lines[idx] or ""
end

------------------------------------------------------------
-- Undo/redo infrastructure
------------------------------------------------------------

local function push_undo(self, entry)
    local stack = self.undo_stack
    stack[#stack + 1] = entry
    -- Trim to max size
    if #stack > MAX_UNDO then
        table.remove(stack, 1)
    end
    -- Clear redo on new edit
    self.redo_stack = {}
    self.modified = true
end

local function break_coalesce(self)
    self._coalesce = nil
end

------------------------------------------------------------
-- Editing operations
------------------------------------------------------------

function Buffer:insert_char(ch)
    local line = self.lines[self.cursor_y]
    local before = line:sub(1, self.cursor_x - 1)
    local after = line:sub(self.cursor_x)
    self.lines[self.cursor_y] = before .. ch .. after

    -- Coalesce consecutive char inserts
    local co = self._coalesce
    if co and co.type == "insert"
       and co.line == self.cursor_y
       and co.col + #co.text == self.cursor_x
       and #co.text < COALESCE_MAX then
        co.text = co.text .. ch
    else
        local entry = {type = "insert", line = self.cursor_y, col = self.cursor_x, text = ch}
        push_undo(self, entry)
        self._coalesce = entry
    end

    self.cursor_x = self.cursor_x + 1
    self.modified = true
end

function Buffer:delete_char()
    local line = self.lines[self.cursor_y]
    if self.cursor_x <= #line then
        local deleted = line:sub(self.cursor_x, self.cursor_x)
        self.lines[self.cursor_y] = line:sub(1, self.cursor_x - 1) .. line:sub(self.cursor_x + 1)
        break_coalesce(self)
        push_undo(self, {type = "delete", line = self.cursor_y, col = self.cursor_x, text = deleted})
    elseif self.cursor_y < #self.lines then
        -- Join with next line
        local next_line = self.lines[self.cursor_y + 1]
        self.lines[self.cursor_y] = line .. next_line
        table.remove(self.lines, self.cursor_y + 1)
        break_coalesce(self)
        push_undo(self, {type = "join", line = self.cursor_y, col = #line + 1, text = next_line})
    end
    self.modified = true
end

function Buffer:backspace()
    if self.cursor_x > 1 then
        local line = self.lines[self.cursor_y]
        local deleted = line:sub(self.cursor_x - 1, self.cursor_x - 1)
        self.lines[self.cursor_y] = line:sub(1, self.cursor_x - 2) .. line:sub(self.cursor_x)
        self.cursor_x = self.cursor_x - 1

        -- Coalesce consecutive backspaces
        local co = self._coalesce
        if co and co.type == "backspace"
           and co.line == self.cursor_y
           and co.col == self.cursor_x + 1
           and #co.text < COALESCE_MAX then
            co.text = deleted .. co.text
            co.col = self.cursor_x
        else
            local entry = {type = "backspace", line = self.cursor_y, col = self.cursor_x, text = deleted}
            push_undo(self, entry)
            self._coalesce = entry
        end
        self.modified = true
    elseif self.cursor_y > 1 then
        -- Join with previous line
        local prev_len = #self.lines[self.cursor_y - 1]
        local cur_line = self.lines[self.cursor_y]
        self.lines[self.cursor_y - 1] = self.lines[self.cursor_y - 1] .. cur_line
        table.remove(self.lines, self.cursor_y)
        self.cursor_y = self.cursor_y - 1
        self.cursor_x = prev_len + 1
        break_coalesce(self)
        push_undo(self, {type = "join_up", line = self.cursor_y, col = prev_len + 1, text = cur_line})
        self.modified = true
    end
end

function Buffer:insert_newline()
    local line = self.lines[self.cursor_y]
    local before = line:sub(1, self.cursor_x - 1)
    local after = line:sub(self.cursor_x)
    self.lines[self.cursor_y] = before
    table.insert(self.lines, self.cursor_y + 1, after)
    break_coalesce(self)
    push_undo(self, {type = "newline", line = self.cursor_y, col = self.cursor_x})
    self.cursor_y = self.cursor_y + 1
    self.cursor_x = 1
    self.scroll_x = 0
    self.modified = true
end

function Buffer:delete_line()
    if #self.lines == 0 then return end
    local deleted = self.lines[self.cursor_y]
    table.remove(self.lines, self.cursor_y)
    if #self.lines == 0 then self.lines[1] = "" end
    if self.cursor_y > #self.lines then self.cursor_y = #self.lines end
    self.cursor_x = 1
    break_coalesce(self)
    push_undo(self, {type = "delete_line", line = self.cursor_y, col = 1, text = deleted})
    self.modified = true
end

function Buffer:insert_tab(use_spaces, tab_size)
    if use_spaces then
        local spaces = tab_size - ((self.cursor_x - 1) % tab_size)
        for _ = 1, spaces do
            self:insert_char(" ")
        end
    else
        self:insert_char("\t")
    end
end

------------------------------------------------------------
-- Auto-indent: copy leading whitespace from current line
------------------------------------------------------------

function Buffer:insert_newline_auto(use_spaces, tab_size)
    local line = self.lines[self.cursor_y]
    local indent = line:match("^(%s*)") or ""
    self:insert_newline()
    -- Insert indent chars one by one (so undo can reverse them)
    for i = 1, #indent do
        self:insert_char(indent:sub(i, i))
    end
end

------------------------------------------------------------
-- Undo / Redo
------------------------------------------------------------

function Buffer:undo()
    if #self.undo_stack == 0 then return false end
    break_coalesce(self)
    local entry = table.remove(self.undo_stack)
    local redo = self.redo_stack
    redo[#redo + 1] = entry

    if entry.type == "insert" then
        -- Remove inserted text
        local line = self.lines[entry.line]
        self.lines[entry.line] = line:sub(1, entry.col - 1) .. line:sub(entry.col + #entry.text)
        self.cursor_y = entry.line
        self.cursor_x = entry.col

    elseif entry.type == "delete" then
        -- Re-insert deleted char
        local line = self.lines[entry.line]
        self.lines[entry.line] = line:sub(1, entry.col - 1) .. entry.text .. line:sub(entry.col)
        self.cursor_y = entry.line
        self.cursor_x = entry.col

    elseif entry.type == "backspace" then
        -- Re-insert backspaced text
        local line = self.lines[entry.line]
        self.lines[entry.line] = line:sub(1, entry.col - 1) .. entry.text .. line:sub(entry.col)
        self.cursor_y = entry.line
        self.cursor_x = entry.col + #entry.text

    elseif entry.type == "newline" then
        -- Re-join the split lines
        local current = self.lines[entry.line]
        local next_line = self.lines[entry.line + 1] or ""
        self.lines[entry.line] = current .. next_line
        table.remove(self.lines, entry.line + 1)
        self.cursor_y = entry.line
        self.cursor_x = entry.col

    elseif entry.type == "join" then
        -- Re-split: restore the joined next line
        local line = self.lines[entry.line]
        self.lines[entry.line] = line:sub(1, entry.col - 1)
        table.insert(self.lines, entry.line + 1, entry.text)
        self.cursor_y = entry.line
        self.cursor_x = entry.col

    elseif entry.type == "join_up" then
        -- Re-split: restore the joined current line
        local line = self.lines[entry.line]
        self.lines[entry.line] = line:sub(1, entry.col - 1)
        table.insert(self.lines, entry.line + 1, entry.text)
        self.cursor_y = entry.line + 1
        self.cursor_x = 1

    elseif entry.type == "delete_line" then
        -- Re-insert deleted line
        table.insert(self.lines, entry.line, entry.text)
        self.cursor_y = entry.line
        self.cursor_x = 1
    end

    self.modified = #self.undo_stack > 0
    return true
end

function Buffer:redo()
    if #self.redo_stack == 0 then return false end
    break_coalesce(self)
    local entry = table.remove(self.redo_stack)
    local undo = self.undo_stack
    undo[#undo + 1] = entry

    if entry.type == "insert" then
        local line = self.lines[entry.line]
        self.lines[entry.line] = line:sub(1, entry.col - 1) .. entry.text .. line:sub(entry.col)
        self.cursor_y = entry.line
        self.cursor_x = entry.col + #entry.text

    elseif entry.type == "delete" then
        local line = self.lines[entry.line]
        self.lines[entry.line] = line:sub(1, entry.col - 1) .. line:sub(entry.col + #entry.text)
        self.cursor_y = entry.line
        self.cursor_x = entry.col

    elseif entry.type == "backspace" then
        local line = self.lines[entry.line]
        self.lines[entry.line] = line:sub(1, entry.col - 1) .. line:sub(entry.col + #entry.text)
        self.cursor_y = entry.line
        self.cursor_x = entry.col

    elseif entry.type == "newline" then
        local line = self.lines[entry.line]
        self.lines[entry.line] = line:sub(1, entry.col - 1)
        table.insert(self.lines, entry.line + 1, line:sub(entry.col))
        self.cursor_y = entry.line + 1
        self.cursor_x = 1

    elseif entry.type == "join" then
        local line = self.lines[entry.line]
        local next_line = self.lines[entry.line + 1] or ""
        self.lines[entry.line] = line .. next_line
        table.remove(self.lines, entry.line + 1)
        self.cursor_y = entry.line
        self.cursor_x = entry.col

    elseif entry.type == "join_up" then
        local line = self.lines[entry.line]
        local next_line = self.lines[entry.line + 1] or ""
        self.lines[entry.line] = line .. next_line
        table.remove(self.lines, entry.line + 1)
        self.cursor_y = entry.line
        self.cursor_x = entry.col

    elseif entry.type == "delete_line" then
        table.remove(self.lines, entry.line)
        if #self.lines == 0 then self.lines[1] = "" end
        if self.cursor_y > #self.lines then self.cursor_y = #self.lines end
        self.cursor_x = 1
    end

    self.modified = true
    return true
end

------------------------------------------------------------
-- Cursor movement
------------------------------------------------------------

function Buffer:move_cursor(dx, dy)
    local total = #self.lines
    if dy ~= 0 then
        self.cursor_y = self.cursor_y + dy
        break_coalesce(self)
    end
    if dx ~= 0 then
        local line_len = #self.lines[self.cursor_y]
        self.cursor_x = self.cursor_x + dx
        -- Wrap to previous/next line
        if self.cursor_x < 1 and self.cursor_y > 1 then
            self.cursor_y = self.cursor_y - 1
            self.cursor_x = #self.lines[self.cursor_y] + 1
        elseif self.cursor_x > line_len + 1 and self.cursor_y < total then
            self.cursor_y = self.cursor_y + 1
            self.cursor_x = 1
        end
        break_coalesce(self)
    end
    self:clamp_cursor()
end

function Buffer:clamp_cursor()
    local total = #self.lines
    if self.cursor_y < 1 then self.cursor_y = 1 end
    if self.cursor_y > total then self.cursor_y = total end
    local line_len = #self.lines[self.cursor_y]
    if self.cursor_x < 1 then self.cursor_x = 1 end
    if self.cursor_x > line_len + 1 then self.cursor_x = line_len + 1 end
end

function Buffer:update_scroll(visible_rows, visible_cols)
    -- Vertical scroll
    if self.cursor_y - 1 < self.scroll_y then
        self.scroll_y = self.cursor_y - 1
    end
    if self.cursor_y - 1 >= self.scroll_y + visible_rows then
        self.scroll_y = self.cursor_y - visible_rows
    end
    if self.scroll_y < 0 then self.scroll_y = 0 end

    -- Horizontal scroll
    if self.cursor_x - 1 < self.scroll_x then
        self.scroll_x = self.cursor_x - 1
    end
    if self.cursor_x - 1 >= self.scroll_x + visible_cols then
        self.scroll_x = self.cursor_x - visible_cols
    end
    if self.scroll_x < 0 then self.scroll_x = 0 end
end

------------------------------------------------------------
-- Find
------------------------------------------------------------

function Buffer:find_next(needle, start_line, start_col)
    if not needle or needle == "" then return nil end
    for y = start_line, #self.lines do
        local from = (y == start_line) and start_col or 1
        local x = self.lines[y]:find(needle, from, true)
        if x then
            return y, x
        end
    end
    -- Wrap around
    for y = 1, start_line do
        local max_col = (y == start_line) and (start_col - 1) or #self.lines[y]
        local x = self.lines[y]:find(needle, 1, true)
        if x and (y < start_line or x < start_col) then
            return y, x
        end
    end
    return nil
end

function Buffer:replace_at(line, col, old_text, new_text)
    local l = self.lines[line]
    if l:sub(col, col + #old_text - 1) == old_text then
        self.lines[line] = l:sub(1, col - 1) .. new_text .. l:sub(col + #old_text)
        break_coalesce(self)
        push_undo(self, {type = "replace", line = line, col = col, text = old_text, new_text = new_text})
        self.modified = true
        return true
    end
    return false
end

------------------------------------------------------------
-- Function scanning (for autocomplete)
------------------------------------------------------------

function Buffer:scan_functions()
    local funcs = {}
    local seen = {}
    for _, line in ipairs(self.lines) do
        -- Match "function name(" and "local function name("
        for name in line:gmatch("function%s+([%w_%.]+)%s*%(") do
            if not seen[name] then
                seen[name] = true
                funcs[#funcs + 1] = name
            end
        end
    end
    table.sort(funcs)
    return funcs
end

return Buffer
