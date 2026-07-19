-- ui_tabs.lua — Tab bar rendering for PicoForge
-- Draws 5 fixed-width tabs: Code, Sprites, SFX, Music, Mem

local Tabs = {}
Tabs.__index = Tabs

-- Tab definitions
Tabs.TABS = {
    {name = "Code",    key = "F1", id = "code"},
    {name = "Sprites", key = "F2", id = "sprites"},
    {name = "SFX",     key = "F3", id = "sfx"},
    {name = "Music",   key = "F4", id = "music"},
    {name = "Mem",     key = "CF5", id = "mem"},  -- Ctrl+F5
}

-- Colors (RGB565)
local BG_ACTIVE   = 0x4A69   -- dark blue-gray
local BG_INACTIVE = 0x2104   -- very dark gray
local FG_ACTIVE   = 0xFFFF   -- white
local FG_INACTIVE = 0x8410   -- mid gray
local BORDER      = 0x4208   -- gray border

function Tabs.new(y, width)
    return setmetatable({
        y = y or 28,
        width = width or 320,
        height = 12,
        active = 1,  -- 1-indexed
    }, Tabs)
end

function Tabs:draw(disp)
    local tab_w = math.floor(self.width / #self.TABS)
    local y = self.y

    -- Background bar
    disp.fillRect(0, y, self.width, self.height, BG_INACTIVE)

    for i, tab in ipairs(self.TABS) do
        local x = (i - 1) * tab_w
        local is_active = (i == self.active)

        if is_active then
            disp.fillRect(x, y, tab_w, self.height, BG_ACTIVE)
        end

        -- Tab label centered
        local fg = is_active and FG_ACTIVE or FG_INACTIVE
        local label = tab.name
        local text_w = #label * 6  -- 6px per char
        local tx = x + math.floor((tab_w - text_w) / 2)
        local ty = y + 2
        disp.drawText(tx, ty, label, fg)

        -- Right border
        if i < #self.TABS then
            disp.drawLine(x + tab_w - 1, y, x + tab_w - 1, y + self.height - 1, BORDER)
        end
    end
end

function Tabs:set_active(index)
    if index >= 1 and index <= #self.TABS then
        self.active = index
    end
end

function Tabs:get_active_id()
    return self.TABS[self.active].id
end

return Tabs
