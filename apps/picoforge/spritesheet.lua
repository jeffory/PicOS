-- spritesheet.lua — Sprite sheet manager for PicoForge
-- A grid of sprites stored as a flat pixel buffer
-- Saved as PNG on disk, with metadata in JSON

local Spritesheet = {}
Spritesheet.__index = Spritesheet

function Spritesheet.new(sprite_w, sprite_h, cols, rows)
    sprite_w = sprite_w or 16
    sprite_h = sprite_h or 16
    cols = cols or 8
    rows = rows or 8

    local self = setmetatable({
        sprite_w = sprite_w,
        sprite_h = sprite_h,
        cols = cols,
        rows = rows,
        sheet_w = sprite_w * cols,
        sheet_h = sprite_h * rows,
        pixels = {},        -- flat pixel buffer for entire sheet
        selected = 0,       -- selected sprite index (0-indexed)
        modified = false,
    }, Spritesheet)

    self:clear()
    return self
end

function Spritesheet:clear()
    for i = 1, self.sheet_w * self.sheet_h do
        self.pixels[i] = 0x0000
    end
end

-- Total sprite count
function Spritesheet:count()
    return self.cols * self.rows
end

-- Get pixel in sheet coordinates
function Spritesheet:get_pixel(x, y)
    if x < 0 or x >= self.sheet_w or y < 0 or y >= self.sheet_h then
        return 0x0000
    end
    return self.pixels[y * self.sheet_w + x + 1] or 0x0000
end

-- Set pixel in sheet coordinates
function Spritesheet:set_pixel(x, y, color)
    if x < 0 or x >= self.sheet_w or y < 0 or y >= self.sheet_h then
        return
    end
    self.pixels[y * self.sheet_w + x + 1] = color
    self.modified = true
end

-- Get the top-left coordinates of a sprite by index (0-indexed)
function Spritesheet:sprite_origin(index)
    local col = index % self.cols
    local row = math.floor(index / self.cols)
    return col * self.sprite_w, row * self.sprite_h
end

-- Extract a single sprite's pixels into a flat table
function Spritesheet:extract_sprite(index)
    local ox, oy = self:sprite_origin(index)
    local data = {}
    for py = 0, self.sprite_h - 1 do
        for px = 0, self.sprite_w - 1 do
            data[#data + 1] = self:get_pixel(ox + px, oy + py)
        end
    end
    return data
end

-- Write a single sprite's pixels from a flat table
function Spritesheet:write_sprite(index, data)
    local ox, oy = self:sprite_origin(index)
    local idx = 1
    for py = 0, self.sprite_h - 1 do
        for px = 0, self.sprite_w - 1 do
            self:set_pixel(ox + px, oy + py, data[idx] or 0x0000)
            idx = idx + 1
        end
    end
    self.modified = true
end

-- Write canvas pixels into the currently selected sprite slot
function Spritesheet:write_from_canvas(canvas)
    self:write_sprite(self.selected, canvas:export_pixels())
end

-- Load a sprite from the sheet into a canvas
function Spritesheet:load_to_canvas(canvas, index)
    index = index or self.selected
    local data = self:extract_sprite(index)
    canvas:import_pixels(data, self.sprite_w, self.sprite_h)
    canvas.modified = false
end

-- Select a sprite by index
function Spritesheet:select(index)
    if index >= 0 and index < self:count() then
        self.selected = index
    end
end

-- Navigate selection
function Spritesheet:move_selection(dx, dy)
    local col = self.selected % self.cols
    local row = math.floor(self.selected / self.cols)
    col = col + dx
    row = row + dy
    if col < 0 then col = 0 end
    if col >= self.cols then col = self.cols - 1 end
    if row < 0 then row = 0 end
    if row >= self.rows then row = self.rows - 1 end
    self.selected = row * self.cols + col
end

------------------------------------------------------------
-- Rendering: mini grid overview
------------------------------------------------------------

function Spritesheet:draw_grid(disp, x, y, cell_size, visible_rows)
    cell_size = cell_size or 10
    visible_rows = visible_rows or self.rows
    local gap = 1

    -- Determine scroll offset to keep selected visible
    local sel_row = math.floor(self.selected / self.cols)
    local scroll = 0
    if sel_row >= visible_rows then
        scroll = sel_row - visible_rows + 1
    end

    for r = 0, visible_rows - 1 do
        local sheet_row = r + scroll
        if sheet_row >= self.rows then break end
        for c = 0, self.cols - 1 do
            local idx = sheet_row * self.cols + c
            local gx = x + c * (cell_size + gap)
            local gy = y + r * (cell_size + gap)

            -- Draw miniature of sprite (just sample center color)
            local ox, oy = self:sprite_origin(idx)
            local center_color = self:get_pixel(
                ox + math.floor(self.sprite_w / 2),
                oy + math.floor(self.sprite_h / 2)
            )
            -- Check if sprite has any non-black pixels
            local has_content = false
            for py = 0, self.sprite_h - 1, math.max(1, math.floor(self.sprite_h / 4)) do
                for px = 0, self.sprite_w - 1, math.max(1, math.floor(self.sprite_w / 4)) do
                    if self:get_pixel(ox + px, oy + py) ~= 0x0000 then
                        has_content = true
                        break
                    end
                end
                if has_content then break end
            end

            if has_content then
                disp.fillRect(gx, gy, cell_size, cell_size, center_color)
            else
                disp.fillRect(gx, gy, cell_size, cell_size, 0x0841)
            end

            -- Highlight selected
            if idx == self.selected then
                disp.drawRect(gx - 1, gy - 1, cell_size + 2, cell_size + 2, 0xFFFF)
            end
        end
    end

    -- Draw index label for selected
    local label = string.format("#%d", self.selected)
    disp.drawText(x, y + visible_rows * (cell_size + gap) + 2, label, 0xD6BA)
end

------------------------------------------------------------
-- Save/Load: convert to/from raw pixel data string
-- Format: sprite_w, sprite_h, cols, rows, then hex pixels
------------------------------------------------------------

function Spritesheet:to_data()
    local parts = {
        string.format("%d,%d,%d,%d", self.sprite_w, self.sprite_h, self.cols, self.rows)
    }
    -- Pack pixels as hex string, 4 hex digits per pixel
    local hex = {}
    for i = 1, self.sheet_w * self.sheet_h do
        hex[#hex + 1] = string.format("%04X", self.pixels[i] or 0)
    end
    parts[#parts + 1] = table.concat(hex)
    return table.concat(parts, "\n")
end

function Spritesheet:from_data(data)
    if not data or #data == 0 then return false end
    local header, hex_data = data:match("^([^\n]+)\n(.+)$")
    if not header then return false end

    local sw, sh, c, r = header:match("^(%d+),(%d+),(%d+),(%d+)$")
    if not sw then return false end

    self.sprite_w = tonumber(sw)
    self.sprite_h = tonumber(sh)
    self.cols = tonumber(c)
    self.rows = tonumber(r)
    self.sheet_w = self.sprite_w * self.cols
    self.sheet_h = self.sprite_h * self.rows

    local idx = 1
    for hex in hex_data:gmatch("(%x%x%x%x)") do
        self.pixels[idx] = tonumber(hex, 16) or 0
        idx = idx + 1
    end
    -- Fill remaining with black
    for i = idx, self.sheet_w * self.sheet_h do
        self.pixels[i] = 0x0000
    end

    self.modified = false
    return true
end

-- Save to file
function Spritesheet:save(fs, path)
    local f = fs.open(path, "w")
    if not f then return false end
    fs.write(f, self:to_data())
    fs.close(f)
    self.modified = false
    return true
end

-- Load from file
function Spritesheet:load(fs, path)
    if not fs.exists(path) then return false end
    local f = fs.open(path, "r")
    if not f then return false end
    local chunks = {}
    while true do
        local data = fs.read(f, 4096)
        if not data or #data == 0 then break end
        chunks[#chunks + 1] = data
    end
    fs.close(f)
    return self:from_data(table.concat(chunks))
end

return Spritesheet
