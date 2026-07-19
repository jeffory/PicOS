-- mem_viewer.lua — Memory/hex viewer for PicoForge
-- View and edit project files in hex + ASCII format

local MemViewer = {}
MemViewer.__index = MemViewer

-- Layout constants
local CONTENT_Y = 40
local SCREEN_W  = 320
local FOOTER_Y  = 302

local GRID_X = 8
local GRID_Y = CONTENT_Y + 14
local ROW_H = 11
local BYTES_PER_ROW = 8    -- 8 bytes per row (fits 320px with hex + ASCII)
local VISIBLE_ROWS = 21

-- Column positions
local COL_ADDR = 0          -- address column
local COL_HEX = 36          -- hex bytes start
local COL_ASCII = 180       -- ASCII display start
local HEX_BYTE_W = 18      -- width per hex byte (2 chars + space)

-- File list panel
local FILES_X = 220
local FILES_Y = CONTENT_Y + 4

function MemViewer.new()
    return setmetatable({
        project_path = nil,
        files = {},          -- list of files in project
        file_idx = 1,        -- selected file (1-indexed)
        data = {},           -- loaded file bytes (1-indexed)
        data_size = 0,
        filename = nil,
        cursor = 0,          -- byte offset (0-indexed)
        scroll = 0,          -- row scroll offset
        editing = false,     -- in hex edit mode
        edit_nibble = 0,     -- 0 = high nibble, 1 = low nibble
        modified = false,
        show_files = false,  -- file list overlay
    }, MemViewer)
end

function MemViewer:init(project_path, fs)
    self.project_path = project_path
    self:scan_files(fs)
    -- Auto-load first file
    if #self.files > 0 then
        self:load_file(fs, self.files[1])
    end
end

function MemViewer:scan_files(fs)
    self.files = {}
    if not self.project_path then return end
    local entries = fs.listDir(self.project_path)
    if entries then
        for _, e in ipairs(entries) do
            if not e.isDir then
                self.files[#self.files + 1] = e.name
            end
        end
        table.sort(self.files)
    end
end

function MemViewer:load_file(fs, filename)
    if not self.project_path then return end
    local path = self.project_path .. "/" .. filename
    if not fs.exists(path) then return end

    local f = fs.open(path, "r")
    if not f then return end

    local chunks = {}
    while true do
        local chunk = fs.read(f, 4096)
        if not chunk or #chunk == 0 then break end
        chunks[#chunks + 1] = chunk
    end
    fs.close(f)

    local raw = table.concat(chunks)
    self.data = {}
    for i = 1, #raw do
        self.data[i] = string.byte(raw, i)
    end
    self.data_size = #raw
    self.filename = filename
    self.cursor = 0
    self.scroll = 0
    self.editing = false
    self.modified = false
end

function MemViewer:save_file(fs)
    if not self.project_path or not self.filename then return false end
    local path = self.project_path .. "/" .. self.filename
    local f = fs.open(path, "w")
    if not f then return false end

    -- Convert bytes back to string in chunks
    local chunk_size = 256
    for i = 1, self.data_size, chunk_size do
        local chars = {}
        for j = i, math.min(i + chunk_size - 1, self.data_size) do
            chars[#chars + 1] = string.char(self.data[j])
        end
        fs.write(f, table.concat(chars))
    end
    fs.close(f)
    self.modified = false
    return true
end

------------------------------------------------------------
-- Input handling
------------------------------------------------------------

function MemViewer:handle_button(pressed, held, char, BTN)
    local is_ctrl = (held & BTN.CTRL) ~= 0
    local function btn(b) return (pressed & b) ~= 0 end

    -- File list overlay
    if self.show_files then
        return self:handle_file_list(pressed, BTN)
    end

    -- Ctrl shortcuts
    if is_ctrl and char then
        local lc = char:lower()
        if lc == "s" then
            return true  -- handled by caller
        elseif lc == "o" then
            self.show_files = true
            return true
        elseif lc == "g" then
            -- Go to address (toggle editing mode)
            self.editing = not self.editing
            self.edit_nibble = 0
            return true
        end
        return false
    end

    -- Arrow navigation
    if btn(BTN.UP) then
        self.cursor = self.cursor - BYTES_PER_ROW
        if self.cursor < 0 then self.cursor = 0 end
        self:_ensure_visible()
        return true
    end
    if btn(BTN.DOWN) then
        self.cursor = self.cursor + BYTES_PER_ROW
        if self.cursor >= self.data_size then
            self.cursor = math.max(0, self.data_size - 1)
        end
        self:_ensure_visible()
        return true
    end
    if btn(BTN.LEFT) then
        if self.cursor > 0 then
            self.cursor = self.cursor - 1
            self.edit_nibble = 0
        end
        self:_ensure_visible()
        return true
    end
    if btn(BTN.RIGHT) then
        if self.cursor < self.data_size - 1 then
            self.cursor = self.cursor + 1
            self.edit_nibble = 0
        end
        self:_ensure_visible()
        return true
    end

    -- Page up/down
    if btn(BTN.FN) then
        if (held & BTN.UP) ~= 0 then
            self.cursor = self.cursor - BYTES_PER_ROW * VISIBLE_ROWS
            if self.cursor < 0 then self.cursor = 0 end
            self:_ensure_visible()
            return true
        end
        if (held & BTN.DOWN) ~= 0 then
            self.cursor = self.cursor + BYTES_PER_ROW * VISIBLE_ROWS
            if self.cursor >= self.data_size then
                self.cursor = math.max(0, self.data_size - 1)
            end
            self:_ensure_visible()
            return true
        end
    end

    -- Tab: toggle edit mode
    if btn(BTN.TAB) then
        self.editing = not self.editing
        self.edit_nibble = 0
        return true
    end

    -- Hex editing
    if self.editing and char then
        return self:handle_hex_input(char)
    end

    -- Escape: exit edit mode or handled by caller
    if btn(BTN.ESC) then
        if self.editing then
            self.editing = false
            return true
        end
        return false  -- let caller handle
    end

    return false
end

function MemViewer:handle_hex_input(char)
    local lc = char:lower()
    local nibble = nil

    if lc >= "0" and lc <= "9" then
        nibble = string.byte(lc) - string.byte("0")
    elseif lc >= "a" and lc <= "f" then
        nibble = 10 + string.byte(lc) - string.byte("a")
    end

    if nibble and self.cursor < self.data_size then
        local byte = self.data[self.cursor + 1] or 0
        if self.edit_nibble == 0 then
            -- High nibble
            byte = (nibble << 4) | (byte & 0x0F)
            self.data[self.cursor + 1] = byte
            self.edit_nibble = 1
        else
            -- Low nibble
            byte = (byte & 0xF0) | nibble
            self.data[self.cursor + 1] = byte
            self.edit_nibble = 0
            -- Advance cursor
            if self.cursor < self.data_size - 1 then
                self.cursor = self.cursor + 1
            end
            self:_ensure_visible()
        end
        self.modified = true
        return true
    end

    return false
end

function MemViewer:handle_file_list(pressed, BTN)
    local function btn(b) return (pressed & b) ~= 0 end

    if btn(BTN.UP) then
        self.file_idx = self.file_idx - 1
        if self.file_idx < 1 then self.file_idx = #self.files end
        return true
    end
    if btn(BTN.DOWN) then
        self.file_idx = self.file_idx + 1
        if self.file_idx > #self.files then self.file_idx = 1 end
        return true
    end
    if btn(BTN.ENTER) then
        if #self.files > 0 then
            -- Would need fs reference — defer to caller pattern
            self.show_files = false
            -- Store selected filename for caller to load
            self._pending_file = self.files[self.file_idx]
        end
        return true
    end
    if btn(BTN.ESC) then
        self.show_files = false
        return true
    end

    return false
end

-- Check if a file load is pending (called by main after handle_button)
function MemViewer:check_pending_load(fs)
    if self._pending_file then
        self:load_file(fs, self._pending_file)
        self._pending_file = nil
    end
end

function MemViewer:_ensure_visible()
    local cursor_row = math.floor(self.cursor / BYTES_PER_ROW)
    if cursor_row < self.scroll then
        self.scroll = cursor_row
    end
    if cursor_row >= self.scroll + VISIBLE_ROWS then
        self.scroll = cursor_row - VISIBLE_ROWS + 1
    end
end

------------------------------------------------------------
-- Rendering
------------------------------------------------------------

function MemViewer:draw(disp)
    disp.fillRect(0, CONTENT_Y, SCREEN_W, FOOTER_Y - CONTENT_Y, 0x0000)

    if self.data_size == 0 then
        disp.drawText(GRID_X, GRID_Y + 20, "No file loaded", 0x7BEF)
        disp.drawText(GRID_X, GRID_Y + 34, "Press Ctrl+O to open", 0x4A69)
    else
        self:draw_hex_grid(disp)
    end

    -- File list overlay
    if self.show_files then
        self:draw_file_list(disp)
    end
end

function MemViewer:draw_hex_grid(disp)
    -- Header
    local hy = GRID_Y - 12
    disp.drawText(GRID_X + COL_ADDR, hy, "ADDR", 0x4A69)
    local hx_labels = ""
    for i = 0, BYTES_PER_ROW - 1 do
        hx_labels = hx_labels .. string.format("%X  ", i)
    end
    disp.drawText(GRID_X + COL_HEX, hy, hx_labels, 0x4A69)
    disp.drawText(GRID_X + COL_ASCII, hy, "ASCII", 0x4A69)

    -- File info (top right)
    if self.filename then
        local info = self.filename
        if self.modified then info = info .. " *" end
        disp.drawText(SCREEN_W - #info * 6 - 4, hy, info, 0xD6BA)
    end

    local total_rows = math.ceil(self.data_size / BYTES_PER_ROW)
    local cursor_row = math.floor(self.cursor / BYTES_PER_ROW)
    local cursor_col = self.cursor % BYTES_PER_ROW

    for i = 0, VISIBLE_ROWS - 1 do
        local row = self.scroll + i
        if row >= total_rows then break end

        local ry = GRID_Y + i * ROW_H
        local addr = row * BYTES_PER_ROW
        local is_cursor_row = (row == cursor_row)

        -- Row highlight
        if is_cursor_row then
            disp.fillRect(GRID_X, ry - 1, SCREEN_W - 16, ROW_H, 0x10A2)
        end

        -- Address
        disp.drawText(GRID_X + COL_ADDR, ry,
            string.format("%04X", addr), 0x4A69)

        -- Hex bytes
        local ascii = {}
        for col = 0, BYTES_PER_ROW - 1 do
            local byte_idx = addr + col
            if byte_idx >= self.data_size then break end

            local byte = self.data[byte_idx + 1] or 0
            local hx = GRID_X + COL_HEX + col * HEX_BYTE_W
            local is_cursor_byte = is_cursor_row and (col == cursor_col)

            -- Hex color
            local hex_color = 0xD6BA
            if byte == 0 then
                hex_color = 0x2945
            elseif byte >= 32 and byte <= 126 then
                hex_color = 0x5EFB
            end

            if is_cursor_byte then
                if self.editing then
                    disp.fillRect(hx - 1, ry - 1, HEX_BYTE_W, ROW_H, 0x4A69)
                    hex_color = 0xFFFF
                    -- Show nibble cursor
                    local nib_x = hx + self.edit_nibble * 6
                    disp.fillRect(nib_x, ry + ROW_H - 2, 6, 1, 0xFFFF)
                else
                    disp.drawRect(hx - 1, ry - 1, HEX_BYTE_W, ROW_H, 0xFFFF)
                    hex_color = 0xFFFF
                end
            end

            disp.drawText(hx, ry, string.format("%02X", byte), hex_color)

            -- ASCII char
            if byte >= 32 and byte <= 126 then
                ascii[#ascii + 1] = string.char(byte)
            else
                ascii[#ascii + 1] = "."
            end
        end

        -- ASCII column
        local ascii_str = table.concat(ascii)
        for ci = 1, #ascii_str do
            local ax = GRID_X + COL_ASCII + (ci - 1) * 6
            local byte_idx = addr + ci - 1
            local is_cursor_byte = is_cursor_row and ((ci - 1) == cursor_col)
            local asc_color = is_cursor_byte and 0xFFFF or 0x7BEF

            disp.drawText(ax, ry, ascii_str:sub(ci, ci), asc_color)
        end
    end

    -- Scrollbar
    if total_rows > VISIBLE_ROWS then
        local bar_h = VISIBLE_ROWS * ROW_H
        local thumb_h = math.max(8, math.floor(bar_h * VISIBLE_ROWS / total_rows))
        local thumb_y = GRID_Y + math.floor((bar_h - thumb_h) * self.scroll / (total_rows - VISIBLE_ROWS))
        disp.fillRect(SCREEN_W - 4, GRID_Y, 2, bar_h, 0x2104)
        disp.fillRect(SCREEN_W - 4, thumb_y, 2, thumb_h, 0x7BEF)
    end
end

function MemViewer:draw_file_list(disp)
    local x, y, w, h = 40, 60, 240, 200
    disp.fillRect(x, y, w, h, 0x10A2)
    disp.drawRect(x, y, w, h, 0x4A69)
    disp.drawText(x + 8, y + 4, "Open File:", 0xFFFF)

    local ly = y + 20
    for i, fname in ipairs(self.files) do
        if ly > y + h - 14 then break end
        local fg = 0xD6BA
        if i == self.file_idx then
            disp.fillRect(x + 4, ly - 1, w - 8, 13, 0x4A69)
            fg = 0xFFFF
        end
        local label = fname
        if fname == self.filename then label = label .. " <" end
        disp.drawText(x + 8, ly, label, fg)
        ly = ly + 14
    end

    if #self.files == 0 then
        disp.drawText(x + 8, ly, "(no files)", 0x7BEF)
    end
end

function MemViewer:get_footer_text()
    local info = ""
    if self.data_size > 0 then
        local byte = self.data[self.cursor + 1] or 0
        info = string.format("%04X: %02X (%d)  %d bytes",
            self.cursor, byte, byte, self.data_size)
    end
    local edit_str = self.editing and " EDIT" or ""
    local mod_str = self.modified and " [+]" or ""
    return string.format("%s%s%s  ^O:Files  Tab:Edit", info, edit_str, mod_str)
end

return MemViewer
