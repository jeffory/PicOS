-- sprite_editor.lua — Sprite editor mode for PicoForge
-- Wires SpriteCanvas + Palette + DrawTools + Spritesheet

local SpriteEditor = {}
SpriteEditor.__index = SpriteEditor

-- Layout constants (320x320 screen, header=28, tabs=12, footer=18)
local CONTENT_Y = 40
local CONTENT_H = 262
local FOOTER_Y  = 302
local SCREEN_W  = 320

-- Regions
local CANVAS_X = 8
local CANVAS_Y = CONTENT_Y + 4
local CANVAS_AREA = 192  -- max canvas render area

local PALETTE_X = 8
local PALETTE_Y = FOOTER_Y - 40

local SHEET_X = 210
local SHEET_Y = CONTENT_Y + 4
local SHEET_CELL = 10
local SHEET_VISIBLE_ROWS = 8

local TOOL_X = 210
local TOOL_Y = SHEET_Y + SHEET_VISIBLE_ROWS * 11 + 16
local TOOL_W = 26
local TOOL_H = 14

local PREVIEW_X = 270
local PREVIEW_Y = FOOTER_Y - 50

function SpriteEditor.new(SpriteCanvas, Palette, DrawTools, Spritesheet)
    local palette = Palette.new()
    palette:init()

    local canvas = SpriteCanvas.new(16, 16)
    canvas:set_origin(CANVAS_X, CANVAS_Y)

    local sheet = Spritesheet.new(16, 16, 8, 8)

    return setmetatable({
        SpriteCanvas = SpriteCanvas,
        Palette = Palette,
        DrawTools = DrawTools,
        Spritesheet = Spritesheet,
        canvas = canvas,
        palette = palette,
        sheet = sheet,
        tool = DrawTools.PENCIL,
        tool_idx = 1,
        drawing = false,     -- true while holding draw button
        drag_start_x = nil,  -- for line/rect/circle
        drag_start_y = nil,
        clipboard = nil,     -- copy/paste region
        project_path = nil,  -- path to save sprites
    }, SpriteEditor)
end

-- Initialize with project path
function SpriteEditor:init(project_path, fs)
    self.project_path = project_path
    -- Try loading existing spritesheet
    local sheet_path = project_path .. "/sprites.spr"
    if fs.exists(sheet_path) then
        self.sheet:load(fs, sheet_path)
        self.sheet:load_to_canvas(self.canvas, 0)
    end
    -- Try loading palette
    local pal_path = project_path .. "/palette.dat"
    if fs.exists(pal_path) then
        local f = fs.open(pal_path, "r")
        if f then
            local data = fs.read(f, 4096)
            fs.close(f)
            if data then self.palette:from_string(data) end
        end
    end
end

-- Save sprites to project
function SpriteEditor:save(fs)
    if not self.project_path then return false end
    -- Save current canvas back to sheet
    self.sheet:write_from_canvas(self.canvas)
    -- Save sheet
    local ok = self.sheet:save(fs, self.project_path .. "/sprites.spr")
    -- Save palette
    local f = fs.open(self.project_path .. "/palette.dat", "w")
    if f then
        fs.write(f, self.palette:to_string())
        fs.close(f)
    end
    return ok
end

-- Switch to a different sprite in the sheet
function SpriteEditor:select_sprite(index)
    -- Save current canvas to sheet first
    if self.canvas.modified then
        self.sheet:write_from_canvas(self.canvas)
    end
    self.sheet:select(index)
    self.sheet:load_to_canvas(self.canvas, index)
end

-- Cycle through tools
function SpriteEditor:next_tool()
    self.tool_idx = (self.tool_idx % #self.DrawTools.ALL) + 1
    self.tool = self.DrawTools.ALL[self.tool_idx]
end

function SpriteEditor:prev_tool()
    self.tool_idx = ((self.tool_idx - 2) % #self.DrawTools.ALL) + 1
    self.tool = self.DrawTools.ALL[self.tool_idx]
end

------------------------------------------------------------
-- Input handling
------------------------------------------------------------

function SpriteEditor:handle_button(pressed, held, char, BTN)
    local canvas = self.canvas
    local is_ctrl = (held & BTN.CTRL) ~= 0

    -- Ctrl shortcuts
    if is_ctrl and char then
        local lc = char:lower()
        if lc == "z" then
            canvas:undo()
            return true
        elseif lc == "y" then
            canvas:redo()
            return true
        elseif lc == "s" then
            return true  -- save handled by caller
        elseif lc == "c" then
            -- Copy current sprite
            self.clipboard = self.DrawTools.copy_region(
                canvas, 0, 0, canvas.width - 1, canvas.height - 1)
            return true
        elseif lc == "v" then
            -- Paste
            if self.clipboard then
                canvas:save_undo()
                self.DrawTools.paste_region(canvas, self.clipboard, 0, 0)
            end
            return true
        end
    end

    local btn = function(b) return (pressed & b) ~= 0 end

    -- Arrow keys: move cursor
    if btn(BTN.UP) then canvas:move_cursor(0, -1); return true end
    if btn(BTN.DOWN) then canvas:move_cursor(0, 1); return true end
    if btn(BTN.LEFT) then canvas:move_cursor(-1, 0); return true end
    if btn(BTN.RIGHT) then canvas:move_cursor(1, 0); return true end

    -- Enter: apply tool at cursor
    if btn(BTN.ENTER) then
        self:apply_tool_at_cursor()
        return true
    end

    -- Backspace: erase pixel at cursor
    if btn(BTN.BACKSPACE) then
        canvas:save_undo()
        canvas:set_pixel(canvas.cursor_x, canvas.cursor_y, self.palette:get_secondary())
        return true
    end

    -- Tab: cycle tool
    if btn(BTN.TAB) then
        if is_ctrl then
            self:prev_tool()
        else
            self:next_tool()
        end
        return true
    end

    -- FN + arrows: navigate spritesheet
    if (held & BTN.FN) ~= 0 then
        if btn(BTN.LEFT) then self:nav_sheet(-1, 0); return true end
        if btn(BTN.RIGHT) then self:nav_sheet(1, 0); return true end
        if btn(BTN.UP) then self:nav_sheet(0, -1); return true end
        if btn(BTN.DOWN) then self:nav_sheet(0, 1); return true end
    end

    -- Number keys via char: select palette color
    if char and not is_ctrl then
        local n = tonumber(char)
        if n then
            -- 1-9 = colors 1-9, 0 = color 10
            local idx = (n == 0) and 10 or n
            self.palette:select(idx)
            return true
        end

        -- Tool shortcuts
        local tool_map = {
            p = self.DrawTools.PENCIL,
            e = self.DrawTools.ERASER,
            f = self.DrawTools.FILL,
            l = self.DrawTools.LINE,
            r = self.DrawTools.RECT,
            c = self.DrawTools.CIRCLE,
            i = self.DrawTools.EYEDROP,
            s = self.DrawTools.SELECT,
        }
        local lc = char:lower()
        if tool_map[lc] then
            self.tool = tool_map[lc]
            for idx, t in ipairs(self.DrawTools.ALL) do
                if t == self.tool then self.tool_idx = idx; break end
            end
            return true
        end

        -- h = flip horizontal, v = flip vertical, w = rotate CW
        if lc == "h" then canvas:flip_h(); return true end
        if lc == "v" then canvas:flip_v(); return true end
        if lc == "w" then canvas:rotate_cw(); return true end

        -- g = toggle grid
        if lc == "g" then canvas.show_grid = not canvas.show_grid; return true end

        -- +/- = zoom
        if char == "+" or char == "=" then
            canvas.zoom = math.min(canvas.zoom + 1, 24)
            return true
        end
        if char == "-" then
            canvas.zoom = math.max(canvas.zoom - 1, 1)
            return true
        end
    end

    return false
end

function SpriteEditor:apply_tool_at_cursor()
    local canvas = self.canvas
    local cx, cy = canvas.cursor_x, canvas.cursor_y
    local color = self.palette:get_selected()
    local tool = self.tool

    if tool == self.DrawTools.PENCIL then
        canvas:save_undo()
        self.DrawTools.pencil(canvas, cx, cy, color)

    elseif tool == self.DrawTools.ERASER then
        canvas:save_undo()
        self.DrawTools.eraser(canvas, cx, cy, self.palette:get_secondary())

    elseif tool == self.DrawTools.FILL then
        canvas:save_undo()
        self.DrawTools.fill(canvas, cx, cy, color)

    elseif tool == self.DrawTools.LINE then
        if not self.drag_start_x then
            self.drag_start_x = cx
            self.drag_start_y = cy
        else
            canvas:save_undo()
            self.DrawTools.line(canvas, self.drag_start_x, self.drag_start_y, cx, cy, color)
            self.drag_start_x = nil
            self.drag_start_y = nil
        end

    elseif tool == self.DrawTools.RECT then
        if not self.drag_start_x then
            self.drag_start_x = cx
            self.drag_start_y = cy
        else
            canvas:save_undo()
            local filled = false  -- outline by default
            self.DrawTools.rect(canvas, self.drag_start_x, self.drag_start_y, cx, cy, color, filled)
            self.drag_start_x = nil
            self.drag_start_y = nil
        end

    elseif tool == self.DrawTools.CIRCLE then
        if not self.drag_start_x then
            self.drag_start_x = cx
            self.drag_start_y = cy
        else
            canvas:save_undo()
            local dx = math.abs(cx - self.drag_start_x)
            local dy = math.abs(cy - self.drag_start_y)
            local r = math.max(dx, dy)
            self.DrawTools.circle(canvas, self.drag_start_x, self.drag_start_y, r, color, false)
            self.drag_start_x = nil
            self.drag_start_y = nil
        end

    elseif tool == self.DrawTools.EYEDROP then
        local picked = canvas:get_pixel(cx, cy)
        local idx = self.palette:find_closest(picked)
        self.palette:select(idx)

    elseif tool == self.DrawTools.SELECT then
        if not self.drag_start_x then
            self.drag_start_x = cx
            self.drag_start_y = cy
        else
            self.clipboard = self.DrawTools.copy_region(
                canvas, self.drag_start_x, self.drag_start_y, cx, cy)
            self.drag_start_x = nil
            self.drag_start_y = nil
        end
    end
end

function SpriteEditor:nav_sheet(dx, dy)
    -- Save current canvas to sheet
    if self.canvas.modified then
        self.sheet:write_from_canvas(self.canvas)
        self.canvas.modified = false
    end
    self.sheet:move_selection(dx, dy)
    self.sheet:load_to_canvas(self.canvas, self.sheet.selected)
end

------------------------------------------------------------
-- Rendering
------------------------------------------------------------

function SpriteEditor:draw(disp)
    -- Clear content area
    disp.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, 0x0000)

    -- Canvas (main editing area)
    self.canvas:draw(disp)

    -- Palette (bottom left)
    self.palette:draw(disp, PALETTE_X, PALETTE_Y, 8)

    -- Spritesheet grid (right side)
    self.sheet:draw_grid(disp, SHEET_X, SHEET_Y, SHEET_CELL, SHEET_VISIBLE_ROWS)

    -- Tool selector (right side, below sheet)
    self:draw_tools(disp)

    -- Preview (bottom right)
    disp.drawText(PREVIEW_X, PREVIEW_Y - 10, "Preview", 0x7BEF)
    disp.drawRect(PREVIEW_X - 1, PREVIEW_Y - 1,
        self.canvas.width + 2, self.canvas.height + 2, 0x4208)
    self.canvas:draw_preview(disp, PREVIEW_X, PREVIEW_Y, 1)

    -- 2x preview
    local p2x = PREVIEW_X
    local p2y = PREVIEW_Y + self.canvas.height + 4
    self.canvas:draw_preview(disp, p2x, p2y, 2)

    -- Drag indicator for line/rect/circle
    if self.drag_start_x then
        local sx, sy = self.canvas:pixel_to_screen(self.drag_start_x, self.drag_start_y)
        local z = self.canvas.zoom
        disp.drawRect(sx, sy, z, z, 0xFFE0)  -- yellow start marker
    end

    -- Cursor position info
    local info = string.format("%d,%d %dx%d",
        self.canvas.cursor_x, self.canvas.cursor_y,
        self.canvas.width, self.canvas.height)
    disp.drawText(CANVAS_X, FOOTER_Y - 52, info, 0x7BEF)
end

function SpriteEditor:draw_tools(disp)
    local x = TOOL_X
    local y = TOOL_Y

    disp.drawText(x, y - 10, "Tools", 0x7BEF)

    for i, tool_id in ipairs(self.DrawTools.ALL) do
        local col = (i - 1) % 4
        local row = math.floor((i - 1) / 4)
        local tx = x + col * (TOOL_W + 2)
        local ty = y + row * (TOOL_H + 2)

        local is_active = (tool_id == self.tool)
        local bg = is_active and 0x4A69 or 0x2104
        local fg = is_active and 0xFFFF or 0x7BEF

        disp.fillRect(tx, ty, TOOL_W, TOOL_H, bg)
        disp.drawRect(tx, ty, TOOL_W, TOOL_H, is_active and 0xFFFF or 0x4208)

        local label = self.DrawTools.LABELS[tool_id] or "?"
        disp.drawText(tx + 2, ty + 3, label, fg)
    end
end

function SpriteEditor:get_footer_text()
    local tool_name = self.DrawTools.LABELS[self.tool] or self.tool
    local mod = self.canvas.modified and "[+]" or ""
    return string.format("%s #%d %s  Tab:Tool  ^S:Save",
        tool_name, self.sheet.selected, mod)
end

return SpriteEditor
