-- palette.lua — Color palette for PicoForge sprite editor
-- 32-color default palette (Pico-8 inspired + extras)

local Palette = {}
Palette.__index = Palette

-- Default 32-color palette (RGB565 values)
-- Row 1: Pico-8 inspired (16 colors)
-- Row 2: Extended useful colors (16 more)
Palette.DEFAULT = {
    -- Row 1: Pico-8 palette
    0x0000,  -- 1  black
    0x194A,  -- 2  dark blue
    0x7809,  -- 3  dark purple
    0x0568,  -- 4  dark green
    0xAB04,  -- 5  brown
    0x5AA9,  -- 6  dark gray
    0xC618,  -- 7  light gray
    0xFFFF,  -- 8  white
    0xF809,  -- 9  red
    0xFD20,  -- 10 orange
    0xFFE0,  -- 11 yellow
    0x07E0,  -- 12 green
    0x2D7F,  -- 13 blue
    0x83B3,  -- 14 indigo
    0xFBB5,  -- 15 pink
    0xFED5,  -- 16 peach

    -- Row 2: Extended
    0x3186,  -- 17 darker gray
    0x8410,  -- 18 medium gray
    0xF800,  -- 19 pure red
    0x07FF,  -- 20 cyan
    0x001F,  -- 21 pure blue
    0xF81F,  -- 22 magenta
    0x7FE0,  -- 23 lime
    0xFFE7,  -- 24 cream
    0x4A49,  -- 25 charcoal
    0xB5B6,  -- 26 silver
    0xC000,  -- 27 dark red
    0x0400,  -- 28 dark green 2
    0x0010,  -- 29 navy
    0x8000,  -- 30 maroon
    0x0208,  -- 31 teal
    0x8010,  -- 32 purple
}

-- Transparent color sentinel
Palette.TRANSPARENT = 0xF81F  -- magenta by convention

function Palette.new()
    return setmetatable({
        colors = {},
        selected = 1,       -- 1-indexed
        secondary = 8,      -- white (right-click / eraser color)
    }, Palette)
end

function Palette:init()
    -- Copy default palette
    for i, c in ipairs(Palette.DEFAULT) do
        self.colors[i] = c
    end
end

function Palette:get_color(index)
    return self.colors[index] or 0x0000
end

function Palette:set_color(index, color)
    if index >= 1 and index <= #self.colors then
        self.colors[index] = color
    end
end

function Palette:get_selected()
    return self.colors[self.selected] or 0x0000
end

function Palette:get_secondary()
    return self.colors[self.secondary] or 0xFFFF
end

function Palette:select(index)
    if index >= 1 and index <= #self.colors then
        self.selected = index
    end
end

function Palette:select_secondary(index)
    if index >= 1 and index <= #self.colors then
        self.secondary = index
    end
end

-- Find closest palette color to a given RGB565 value
function Palette:find_closest(color)
    local best_idx = 1
    local best_dist = math.maxinteger
    for i, c in ipairs(self.colors) do
        if c == color then return i end
        -- Simple color distance in RGB565
        local r1 = (color >> 11) & 0x1F
        local g1 = (color >> 5) & 0x3F
        local b1 = color & 0x1F
        local r2 = (c >> 11) & 0x1F
        local g2 = (c >> 5) & 0x3F
        local b2 = c & 0x1F
        local dist = (r1-r2)*(r1-r2) + (g1-g2)*(g1-g2) + (b1-b2)*(b1-b2)
        if dist < best_dist then
            best_dist = dist
            best_idx = i
        end
    end
    return best_idx
end

-- Draw the palette UI
-- Layout: 2 rows of 16 swatches, with selected highlighted
function Palette:draw(disp, x, y, swatch_size)
    swatch_size = swatch_size or 12
    local cols = 16
    local rows = 2
    local gap = 1

    for i, color in ipairs(self.colors) do
        local row = math.floor((i - 1) / cols)
        local col = (i - 1) % cols
        local sx = x + col * (swatch_size + gap)
        local sy = y + row * (swatch_size + gap)

        disp.fillRect(sx, sy, swatch_size, swatch_size, color)

        -- Highlight selected
        if i == self.selected then
            disp.drawRect(sx - 1, sy - 1, swatch_size + 2, swatch_size + 2, 0xFFFF)
        elseif i == self.secondary then
            disp.drawRect(sx - 1, sy - 1, swatch_size + 2, swatch_size + 2, 0x7BEF)
        end
    end

    -- Show current colors
    local info_x = x
    local info_y = y + rows * (swatch_size + gap) + 4
    disp.fillRect(info_x, info_y, 20, 12, self:get_selected())
    disp.drawRect(info_x, info_y, 20, 12, 0xFFFF)
    disp.drawText(info_x + 24, info_y + 2, "FG", 0xD6BA)
    disp.fillRect(info_x + 44, info_y, 20, 12, self:get_secondary())
    disp.drawRect(info_x + 44, info_y, 20, 12, 0x7BEF)
    disp.drawText(info_x + 68, info_y + 2, "BG", 0xD6BA)
end

-- Handle palette click — returns true if a swatch was clicked
function Palette:handle_click(click_x, click_y, base_x, base_y, swatch_size, is_secondary)
    swatch_size = swatch_size or 12
    local cols = 16
    local gap = 1

    for i = 1, #self.colors do
        local row = math.floor((i - 1) / cols)
        local col = (i - 1) % cols
        local sx = base_x + col * (swatch_size + gap)
        local sy = base_y + row * (swatch_size + gap)

        if click_x >= sx and click_x < sx + swatch_size
           and click_y >= sy and click_y < sy + swatch_size then
            if is_secondary then
                self.secondary = i
            else
                self.selected = i
            end
            return true
        end
    end
    return false
end

-- Save palette to string (for project.json)
function Palette:to_string()
    local parts = {}
    for _, c in ipairs(self.colors) do
        parts[#parts + 1] = string.format("%04X", c)
    end
    return table.concat(parts, ",")
end

-- Load palette from string
function Palette:from_string(str)
    if not str or #str == 0 then return end
    local i = 1
    for hex in str:gmatch("(%x+)") do
        self.colors[i] = tonumber(hex, 16) or 0
        i = i + 1
    end
end

return Palette
