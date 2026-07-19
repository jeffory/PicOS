-- sprite_canvas.lua — Pixel canvas for sprite editing
-- Maintains its own pixel buffer (Lua table of RGB565 values)

local SpriteCanvas = {}
SpriteCanvas.__index = SpriteCanvas

-- Supported sprite sizes
SpriteCanvas.SIZES = {8, 16, 32}

function SpriteCanvas.new(width, height)
    width = width or 16
    height = height or 16
    local self = setmetatable({
        width = width,
        height = height,
        pixels = {},       -- 1D array, row-major, 0-indexed coords
        zoom = 1,          -- pixels per canvas pixel on screen
        show_grid = true,
        origin_x = 0,      -- screen position of top-left corner
        origin_y = 0,
        cursor_x = 0,      -- cursor position in pixel coords (0-indexed)
        cursor_y = 0,
        modified = false,
        undo_stack = {},
        redo_stack = {},
    }, SpriteCanvas)
    self:clear(0x0000)
    self:auto_zoom()
    return self
end

-- Clear all pixels to a color
function SpriteCanvas:clear(color)
    color = color or 0x0000
    for i = 1, self.width * self.height do
        self.pixels[i] = color
    end
    self.modified = true
end

-- Get pixel at (x, y), 0-indexed
function SpriteCanvas:get_pixel(x, y)
    if x < 0 or x >= self.width or y < 0 or y >= self.height then
        return 0x0000
    end
    return self.pixels[y * self.width + x + 1] or 0x0000
end

-- Set pixel at (x, y), 0-indexed
function SpriteCanvas:set_pixel(x, y, color)
    if x < 0 or x >= self.width or y < 0 or y >= self.height then
        return
    end
    local idx = y * self.width + x + 1
    if self.pixels[idx] ~= color then
        self.pixels[idx] = color
        self.modified = true
    end
end

-- Auto-calculate zoom to fit in available area
function SpriteCanvas:auto_zoom()
    -- Target: fit in ~192px area (leaving room for palette and tools)
    local max_dim = math.max(self.width, self.height)
    self.zoom = math.floor(192 / max_dim)
    if self.zoom < 1 then self.zoom = 1 end
    if self.zoom > 24 then self.zoom = 24 end
end

-- Set screen origin for rendering
function SpriteCanvas:set_origin(x, y)
    self.origin_x = x
    self.origin_y = y
end

-- Convert screen coords to pixel coords
function SpriteCanvas:screen_to_pixel(sx, sy)
    local px = math.floor((sx - self.origin_x) / self.zoom)
    local py = math.floor((sy - self.origin_y) / self.zoom)
    return px, py
end

-- Convert pixel coords to screen coords (top-left of pixel)
function SpriteCanvas:pixel_to_screen(px, py)
    return self.origin_x + px * self.zoom, self.origin_y + py * self.zoom
end

-- Move cursor within bounds
function SpriteCanvas:move_cursor(dx, dy)
    self.cursor_x = self.cursor_x + dx
    self.cursor_y = self.cursor_y + dy
    if self.cursor_x < 0 then self.cursor_x = 0 end
    if self.cursor_x >= self.width then self.cursor_x = self.width - 1 end
    if self.cursor_y < 0 then self.cursor_y = 0 end
    if self.cursor_y >= self.height then self.cursor_y = self.height - 1 end
end

------------------------------------------------------------
-- Undo/redo: snapshot-based (small sprites = cheap)
------------------------------------------------------------

local MAX_UNDO = 50

function SpriteCanvas:save_undo()
    local snapshot = {}
    for i = 1, self.width * self.height do
        snapshot[i] = self.pixels[i]
    end
    local stack = self.undo_stack
    stack[#stack + 1] = snapshot
    if #stack > MAX_UNDO then
        table.remove(stack, 1)
    end
    self.redo_stack = {}
end

function SpriteCanvas:undo()
    if #self.undo_stack == 0 then return false end
    -- Save current state to redo
    local current = {}
    for i = 1, self.width * self.height do
        current[i] = self.pixels[i]
    end
    self.redo_stack[#self.redo_stack + 1] = current
    -- Restore
    local snapshot = table.remove(self.undo_stack)
    for i = 1, self.width * self.height do
        self.pixels[i] = snapshot[i]
    end
    self.modified = true
    return true
end

function SpriteCanvas:redo()
    if #self.redo_stack == 0 then return false end
    local current = {}
    for i = 1, self.width * self.height do
        current[i] = self.pixels[i]
    end
    self.undo_stack[#self.undo_stack + 1] = current
    local snapshot = table.remove(self.redo_stack)
    for i = 1, self.width * self.height do
        self.pixels[i] = snapshot[i]
    end
    self.modified = true
    return true
end

------------------------------------------------------------
-- Rendering
------------------------------------------------------------

function SpriteCanvas:draw(disp)
    local z = self.zoom
    local ox = self.origin_x
    local oy = self.origin_y

    -- Draw pixels
    for py = 0, self.height - 1 do
        for px = 0, self.width - 1 do
            local color = self:get_pixel(px, py)
            local sx = ox + px * z
            local sy = oy + py * z
            if z >= 3 then
                disp.fillRect(sx, sy, z, z, color)
            else
                -- For small zoom, setPixel is faster
                for dy = 0, z - 1 do
                    for dx = 0, z - 1 do
                        disp.setPixel(sx + dx, sy + dy, color)
                    end
                end
            end
        end
    end

    -- Grid overlay (when zoomed in enough)
    if self.show_grid and z >= 4 then
        local grid_color = 0x2104  -- very dark gray
        for px = 0, self.width do
            local sx = ox + px * z
            disp.drawLine(sx, oy, sx, oy + self.height * z, grid_color)
        end
        for py = 0, self.height do
            local sy = oy + py * z
            disp.drawLine(ox, sy, ox + self.width * z, sy, grid_color)
        end
    end

    -- Cursor highlight
    local cx_screen = ox + self.cursor_x * z
    local cy_screen = oy + self.cursor_y * z
    disp.drawRect(cx_screen, cy_screen, z, z, 0xFFFF)
    if z >= 6 then
        disp.drawRect(cx_screen + 1, cy_screen + 1, z - 2, z - 2, 0x0000)
    end
end

-- Draw a small preview of the sprite at 1:1 scale
function SpriteCanvas:draw_preview(disp, x, y, scale)
    scale = scale or 1
    for py = 0, self.height - 1 do
        for px = 0, self.width - 1 do
            local color = self:get_pixel(px, py)
            if scale == 1 then
                disp.setPixel(x + px, y + py, color)
            else
                disp.fillRect(x + px * scale, y + py * scale, scale, scale, color)
            end
        end
    end
end

------------------------------------------------------------
-- Import/Export
------------------------------------------------------------

-- Export pixel data as flat table (for PNG saving or spritesheet)
function SpriteCanvas:export_pixels()
    local data = {}
    for i = 1, self.width * self.height do
        data[i] = self.pixels[i]
    end
    return data
end

-- Import pixel data from flat table
function SpriteCanvas:import_pixels(data, w, h)
    if w then self.width = w end
    if h then self.height = h end
    for i = 1, self.width * self.height do
        self.pixels[i] = data[i] or 0x0000
    end
    self.modified = true
    self:auto_zoom()
end

-- Flip horizontal
function SpriteCanvas:flip_h()
    self:save_undo()
    for y = 0, self.height - 1 do
        for x = 0, math.floor(self.width / 2) - 1 do
            local rx = self.width - 1 - x
            local idx_l = y * self.width + x + 1
            local idx_r = y * self.width + rx + 1
            self.pixels[idx_l], self.pixels[idx_r] = self.pixels[idx_r], self.pixels[idx_l]
        end
    end
    self.modified = true
end

-- Flip vertical
function SpriteCanvas:flip_v()
    self:save_undo()
    for y = 0, math.floor(self.height / 2) - 1 do
        local by = self.height - 1 - y
        for x = 0, self.width - 1 do
            local idx_t = y * self.width + x + 1
            local idx_b = by * self.width + x + 1
            self.pixels[idx_t], self.pixels[idx_b] = self.pixels[idx_b], self.pixels[idx_t]
        end
    end
    self.modified = true
end

-- Rotate 90 degrees clockwise (only for square sprites)
function SpriteCanvas:rotate_cw()
    if self.width ~= self.height then return end
    self:save_undo()
    local n = self.width
    local new_pixels = {}
    for y = 0, n - 1 do
        for x = 0, n - 1 do
            new_pixels[x * n + (n - 1 - y) + 1] = self:get_pixel(x, y)
        end
    end
    for i = 1, n * n do
        self.pixels[i] = new_pixels[i]
    end
    self.modified = true
end

return SpriteCanvas
