-- draw_tools.lua — Drawing tools for PicoForge sprite editor

local DrawTools = {}

-- Tool IDs
DrawTools.PENCIL    = "pencil"
DrawTools.ERASER    = "eraser"
DrawTools.FILL      = "fill"
DrawTools.LINE      = "line"
DrawTools.RECT      = "rect"
DrawTools.CIRCLE    = "circle"
DrawTools.EYEDROP   = "eyedrop"
DrawTools.SELECT    = "select"

DrawTools.ALL = {
    DrawTools.PENCIL, DrawTools.ERASER, DrawTools.FILL, DrawTools.LINE,
    DrawTools.RECT, DrawTools.CIRCLE, DrawTools.EYEDROP, DrawTools.SELECT,
}

DrawTools.LABELS = {
    [DrawTools.PENCIL]  = "Pen",
    [DrawTools.ERASER]  = "Ers",
    [DrawTools.FILL]    = "Fil",
    [DrawTools.LINE]    = "Lin",
    [DrawTools.RECT]    = "Rec",
    [DrawTools.CIRCLE]  = "Cir",
    [DrawTools.EYEDROP] = "Eye",
    [DrawTools.SELECT]  = "Sel",
}

------------------------------------------------------------
-- Pencil: set single pixel
------------------------------------------------------------

function DrawTools.pencil(canvas, x, y, color)
    canvas:set_pixel(x, y, color)
end

------------------------------------------------------------
-- Eraser: set pixel to secondary/background color
------------------------------------------------------------

function DrawTools.eraser(canvas, x, y, bg_color)
    canvas:set_pixel(x, y, bg_color)
end

------------------------------------------------------------
-- Fill: flood fill from (x, y) with color
------------------------------------------------------------

function DrawTools.fill(canvas, x, y, color)
    local target = canvas:get_pixel(x, y)
    if target == color then return end

    local w = canvas.width
    local h = canvas.height
    local stack = {{x, y}}

    while #stack > 0 do
        local pos = table.remove(stack)
        local px, py = pos[1], pos[2]

        if px >= 0 and px < w and py >= 0 and py < h
           and canvas:get_pixel(px, py) == target then
            canvas:set_pixel(px, py, color)
            stack[#stack + 1] = {px - 1, py}
            stack[#stack + 1] = {px + 1, py}
            stack[#stack + 1] = {px, py - 1}
            stack[#stack + 1] = {px, py + 1}
        end
    end
end

------------------------------------------------------------
-- Line: Bresenham from (x0,y0) to (x1,y1)
------------------------------------------------------------

function DrawTools.line(canvas, x0, y0, x1, y1, color)
    local dx = math.abs(x1 - x0)
    local dy = -math.abs(y1 - y0)
    local sx = x0 < x1 and 1 or -1
    local sy = y0 < y1 and 1 or -1
    local err = dx + dy

    while true do
        canvas:set_pixel(x0, y0, color)
        if x0 == x1 and y0 == y1 then break end
        local e2 = 2 * err
        if e2 >= dy then
            err = err + dy
            x0 = x0 + sx
        end
        if e2 <= dx then
            err = err + dx
            y0 = y0 + sy
        end
    end
end

------------------------------------------------------------
-- Rectangle: outline or filled
------------------------------------------------------------

function DrawTools.rect(canvas, x0, y0, x1, y1, color, filled)
    local lx = math.min(x0, x1)
    local ly = math.min(y0, y1)
    local rx = math.max(x0, x1)
    local ry = math.max(y0, y1)

    if filled then
        for py = ly, ry do
            for px = lx, rx do
                canvas:set_pixel(px, py, color)
            end
        end
    else
        for px = lx, rx do
            canvas:set_pixel(px, ly, color)
            canvas:set_pixel(px, ry, color)
        end
        for py = ly, ry do
            canvas:set_pixel(lx, py, color)
            canvas:set_pixel(rx, py, color)
        end
    end
end

------------------------------------------------------------
-- Circle: midpoint algorithm
------------------------------------------------------------

function DrawTools.circle(canvas, cx, cy, r, color, filled)
    if r <= 0 then
        canvas:set_pixel(cx, cy, color)
        return
    end

    local function plot4(x, y)
        if filled then
            for px = cx - x, cx + x do
                canvas:set_pixel(px, cy + y, color)
                canvas:set_pixel(px, cy - y, color)
            end
            for px = cx - y, cx + y do
                canvas:set_pixel(px, cy + x, color)
                canvas:set_pixel(px, cy - x, color)
            end
        else
            canvas:set_pixel(cx + x, cy + y, color)
            canvas:set_pixel(cx - x, cy + y, color)
            canvas:set_pixel(cx + x, cy - y, color)
            canvas:set_pixel(cx - x, cy - y, color)
            canvas:set_pixel(cx + y, cy + x, color)
            canvas:set_pixel(cx - y, cy + x, color)
            canvas:set_pixel(cx + y, cy - x, color)
            canvas:set_pixel(cx - y, cy - x, color)
        end
    end

    local x = 0
    local y = r
    local d = 1 - r
    plot4(x, y)
    while x < y do
        x = x + 1
        if d < 0 then
            d = d + 2 * x + 1
        else
            y = y - 1
            d = d + 2 * (x - y) + 1
        end
        plot4(x, y)
    end
end

------------------------------------------------------------
-- Selection: copy/paste rectangle region
------------------------------------------------------------

-- Copy region from canvas into a table
function DrawTools.copy_region(canvas, x0, y0, x1, y1)
    local lx = math.min(x0, x1)
    local ly = math.min(y0, y1)
    local rx = math.max(x0, x1)
    local ry = math.max(y0, y1)
    local w = rx - lx + 1
    local h = ry - ly + 1
    local data = {}
    for py = ly, ry do
        for px = lx, rx do
            data[#data + 1] = canvas:get_pixel(px, py)
        end
    end
    return {x = lx, y = ly, w = w, h = h, data = data}
end

-- Paste region onto canvas
function DrawTools.paste_region(canvas, region, dx, dy)
    if not region then return end
    local idx = 1
    for py = 0, region.h - 1 do
        for px = 0, region.w - 1 do
            canvas:set_pixel(dx + px, dy + py, region.data[idx])
            idx = idx + 1
        end
    end
end

return DrawTools
