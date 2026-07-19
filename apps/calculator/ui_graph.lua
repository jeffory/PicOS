-- ui_graph.lua — Graph tab UI for PicOS calculator
-- Handles input mode (expression entry), view mode (graph rendering),
-- trace mode (cursor on curve), and table mode (x/y columns).

local Graph = require("calc_graph")

local UIGraph = {}

-- State
local graph = Graph.new()
local mode = "input"        -- "input", "view", "trace", "table"
local trace_px = 140        -- trace cursor pixel column (within plot)
local trace_slot = 1        -- which function trace follows
local table_start = -5
local table_step = 1
local table_scroll = 0

-- Plot region constants
local PLOT_X = 24
local PLOT_W = 272
local PLOT_Y = 40
local PLOT_H = 220

-- Colors (set during draw from caller)
local FUNC_COLORS  -- set by init or draw

local function init_colors(disp)
    if FUNC_COLORS then return end
    FUNC_COLORS = {
        disp.CYAN,
        disp.rgb(100, 220, 100),   -- green
        disp.YELLOW,
        disp.rgb(220, 100, 220),   -- magenta
    }
end

---------------------------------------------------------------------------
-- Input mode grid
---------------------------------------------------------------------------

local function input_grid()
    return {
        {{"y1","cmd:graph_slot1"},{"y2","cmd:graph_slot2"},{"y3","cmd:graph_slot3"},{"y4","cmd:graph_slot4"},{"Clr","cmd:graph_clr"},{"View","cmd:graph_view"}},
        {{"sin","ins:sin("},{"cos","ins:cos("},{"tan","ins:tan("},{"x","ins:x"},{"^","ins:^"},{"(","ins:("}},
        {{"log","ins:log("},{"ln","ins:ln("},{"sqrt","ins:sqrt("},{"abs","ins:abs("},{"pi","c:pi"},{")","ins:)"}},
        {{"7","d:7"},{"8","d:8"},{"9","d:9"},{"*","ins:*"},{"/","ins:/"},{"DEL","cmd:bs"}},
        {{"4","d:4"},{"5","d:5"},{"6","d:6"},{"+","ins:+"},{"-","ins:-"},{"C","cmd:clear"}},
        {{"1","d:1"},{"2","d:2"},{"3","d:3"},{".","d:."},{"0","d:0"},{"OK","cmd:graph_set"}},
    }
end

---------------------------------------------------------------------------
-- Public API
---------------------------------------------------------------------------

function UIGraph.get_mode()
    return mode
end

function UIGraph.get_graph()
    return graph
end

function UIGraph.get_grid()
    if mode == "input" then
        return input_grid()
    end
    -- View/trace/table modes don't use the button grid
    return input_grid()
end

function UIGraph.get_active_expression()
    return graph.expressions[graph.active_slot] or ""
end

function UIGraph.set_active_expression(expr)
    graph.expressions[graph.active_slot] = expr or ""
end

--- Handle graph-specific actions.
-- Returns (handled, new_expression) or (false) if not handled.
function UIGraph.handle_action(action, expression)
    local kind, val = action:match("^(%a+):(.+)$")
    if not kind then return false end

    if kind == "cmd" then
        if val == "graph_slot1" then
            graph.expressions[graph.active_slot] = expression
            graph.active_slot = 1
            return true, graph.expressions[1]
        elseif val == "graph_slot2" then
            graph.expressions[graph.active_slot] = expression
            graph.active_slot = 2
            return true, graph.expressions[2]
        elseif val == "graph_slot3" then
            graph.expressions[graph.active_slot] = expression
            graph.active_slot = 3
            return true, graph.expressions[3]
        elseif val == "graph_slot4" then
            graph.expressions[graph.active_slot] = expression
            graph.active_slot = 4
            return true, graph.expressions[4]
        elseif val == "graph_set" then
            graph.expressions[graph.active_slot] = expression
            graph:invalidate()
            return true, expression
        elseif val == "graph_clr" then
            graph.expressions[graph.active_slot] = ""
            graph:invalidate()
            return true, ""
        elseif val == "graph_view" then
            graph.expressions[graph.active_slot] = expression
            graph:invalidate()
            mode = "view"
            return true, expression
        elseif val == "graph_input" then
            mode = "input"
            return true, graph.expressions[graph.active_slot]
        elseif val == "graph_trace" then
            mode = "trace"
            trace_px = math.floor(PLOT_W / 2)
            trace_slot = graph.active_slot
            return true, expression
        elseif val == "graph_table" then
            mode = "table"
            table_scroll = 0
            return true, expression
        elseif val == "graph_reset" then
            graph:reset()
            return true, expression
        elseif val == "graph_zoom_in" then
            graph:zoom(1 / 1.5)
            return true, expression
        elseif val == "graph_zoom_out" then
            graph:zoom(1.5)
            return true, expression
        end
    end

    return false
end

--- Handle view/trace/table mode input (arrow keys, etc.).
-- pressed = button bitmask, input = picocalc.input module
-- Returns true if handled.
function UIGraph.handle_view_input(pressed, input, expression)
    if mode == "view" then
        if pressed & input.BTN_LEFT ~= 0 then
            graph:pan(-0.1, 0)
            return true
        elseif pressed & input.BTN_RIGHT ~= 0 then
            graph:pan(0.1, 0)
            return true
        elseif pressed & input.BTN_UP ~= 0 then
            graph:pan(0, 0.1)
            return true
        elseif pressed & input.BTN_DOWN ~= 0 then
            graph:pan(0, -0.1)
            return true
        elseif pressed & input.BTN_ENTER ~= 0 then
            mode = "trace"
            trace_px = math.floor(PLOT_W / 2)
            return true
        end
    elseif mode == "trace" then
        if pressed & input.BTN_LEFT ~= 0 then
            trace_px = math.max(0, trace_px - 1)
            return true
        elseif pressed & input.BTN_RIGHT ~= 0 then
            trace_px = math.min(PLOT_W - 1, trace_px + 1)
            return true
        elseif pressed & input.BTN_UP ~= 0 then
            -- Cycle to next active slot
            for i = 1, 4 do
                local s = ((trace_slot - 1 + i) % 4) + 1
                if graph.expressions[s] ~= "" then
                    trace_slot = s
                    break
                end
            end
            return true
        elseif pressed & input.BTN_DOWN ~= 0 then
            for i = 1, 4 do
                local s = ((trace_slot - 1 - i) % 4) + 1
                if graph.expressions[s] ~= "" then
                    trace_slot = s
                    break
                end
            end
            return true
        elseif pressed & input.BTN_ENTER ~= 0 then
            mode = "view"
            return true
        end
    elseif mode == "table" then
        if pressed & input.BTN_UP ~= 0 then
            table_scroll = table_scroll - 1
            return true
        elseif pressed & input.BTN_DOWN ~= 0 then
            table_scroll = table_scroll + 1
            return true
        elseif pressed & input.BTN_LEFT ~= 0 then
            table_step = table_step / 2
            if table_step < 0.001 then table_step = 0.001 end
            return true
        elseif pressed & input.BTN_RIGHT ~= 0 then
            table_step = table_step * 2
            if table_step > 100 then table_step = 100 end
            return true
        elseif pressed & input.BTN_ENTER ~= 0 then
            mode = "view"
            return true
        end
    end
    return false
end

--- Handle char input in view/trace/table modes.
-- Returns true if handled.
function UIGraph.handle_view_char(ch, expression)
    if mode == "view" then
        if ch == "i" or ch == "I" then
            mode = "input"
            return true, graph.expressions[graph.active_slot]
        elseif ch == "t" or ch == "T" then
            mode = "table"
            table_scroll = 0
            return true, expression
        elseif ch == "r" or ch == "R" then
            graph:reset()
            return true, expression
        elseif ch == "+" or ch == "=" then
            graph:zoom(1 / 1.5)
            return true, expression
        elseif ch == "-" then
            graph:zoom(1.5)
            return true, expression
        end
    elseif mode == "trace" then
        if ch == "\27" then  -- Escape
            mode = "view"
            return true, expression
        end
    elseif mode == "table" then
        if ch == "\27" then
            mode = "view"
            return true, expression
        end
    end
    return false
end

---------------------------------------------------------------------------
-- Drawing: Input mode display area
---------------------------------------------------------------------------

function UIGraph.draw_input_display(disp, colors, layout)
    init_colors(disp)
    local y = layout.display_top
    local h = layout.display_h

    disp.fillRect(0, y, 320, h, colors.display_bg)

    disp.setFont(0)
    for i = 1, 4 do
        local expr = graph.expressions[i] or ""
        local label = "y" .. i .. "="
        local iy = y + 2 + (i - 1) * 12
        local col = (i == graph.active_slot) and FUNC_COLORS[i] or colors.dim
        if expr == "" then expr = "..." end
        local text = label .. expr
        -- Truncate if too long
        local max_chars = 48
        if #text > max_chars then text = text:sub(1, max_chars - 2) .. ".." end
        disp.drawText(4, iy, text, col, colors.display_bg)
    end

    disp.fillRect(0, y + h - 1, 320, 1, disp.rgb(40, 50, 70))
end

---------------------------------------------------------------------------
-- Drawing: View mode (the graph)
---------------------------------------------------------------------------

function UIGraph.draw_view(disp, engine)
    init_colors(disp)
    local vp = graph.viewport

    -- Background
    disp.fillRect(0, 34, 320, 286, disp.rgb(5, 8, 15))

    -- Grid lines
    local xtick = Graph.nice_tick(vp.xmax - vp.xmin)
    local ytick = Graph.nice_tick(vp.ymax - vp.ymin)
    local grid_col = disp.rgb(25, 30, 45)
    local axis_col = disp.rgb(60, 70, 90)
    local label_col = disp.rgb(80, 90, 110)

    disp.setFont(0)

    -- Vertical grid lines
    local x0 = math.ceil(vp.xmin / xtick) * xtick
    while x0 <= vp.xmax do
        local px = math.floor(graph:x_to_px(x0, PLOT_X, PLOT_W))
        if px >= PLOT_X and px <= PLOT_X + PLOT_W then
            disp.fillRect(px, PLOT_Y, 1, PLOT_H, grid_col)
            -- Label
            local lbl = string.format("%g", x0)
            disp.drawText(px - disp.textWidth(lbl) // 2, PLOT_Y + PLOT_H + 2, lbl, label_col, disp.rgb(5, 8, 15))
        end
        x0 = x0 + xtick
    end

    -- Horizontal grid lines
    local y0 = math.ceil(vp.ymin / ytick) * ytick
    while y0 <= vp.ymax do
        local py = math.floor(graph:y_to_px(y0, PLOT_Y, PLOT_H))
        if py >= PLOT_Y and py <= PLOT_Y + PLOT_H then
            disp.fillRect(PLOT_X, py, PLOT_W, 1, grid_col)
            -- Label
            local lbl = string.format("%g", y0)
            disp.drawText(PLOT_X - disp.textWidth(lbl) - 3, py - 4, lbl, label_col, disp.rgb(5, 8, 15))
        end
        y0 = y0 + ytick
    end

    -- Axes (thicker, brighter)
    local ax_px = math.floor(graph:x_to_px(0, PLOT_X, PLOT_W))
    if ax_px >= PLOT_X and ax_px <= PLOT_X + PLOT_W then
        disp.fillRect(ax_px, PLOT_Y, 1, PLOT_H, axis_col)
    end
    local ay_py = math.floor(graph:y_to_px(0, PLOT_Y, PLOT_H))
    if ay_py >= PLOT_Y and ay_py <= PLOT_Y + PLOT_H then
        disp.fillRect(PLOT_X, ay_py, PLOT_W, 1, axis_col)
    end

    -- Function curves
    for slot = 1, 4 do
        if graph.expressions[slot] ~= "" then
            local color = FUNC_COLORS[slot]
            local points = graph:eval_cached(engine, slot, PLOT_X, PLOT_W)
            local prev_py = nil
            for px = 0, PLOT_W - 1 do
                local y_val = points[px]
                if y_val then
                    local py = math.floor(graph:y_to_px(y_val, PLOT_Y, PLOT_H))
                    if py >= PLOT_Y and py <= PLOT_Y + PLOT_H then
                        if prev_py and math.abs(py - prev_py) < PLOT_H / 2 then
                            -- Draw line segment
                            local y1, y2 = prev_py, py
                            if y1 > y2 then y1, y2 = y2, y1 end
                            for ly = y1, y2 do
                                if ly >= PLOT_Y and ly <= PLOT_Y + PLOT_H then
                                    disp.fillRect(PLOT_X + px, ly, 1, 1, color)
                                end
                            end
                        else
                            disp.fillRect(PLOT_X + px, py, 1, 1, color)
                        end
                        prev_py = py
                    else
                        prev_py = nil
                    end
                else
                    prev_py = nil
                end
            end
        end
    end

    -- Legend (top-left)
    disp.setFont(0)
    local ly = PLOT_Y + 2
    for slot = 1, 4 do
        if graph.expressions[slot] ~= "" then
            local label = "y" .. slot .. "=" .. graph.expressions[slot]
            if #label > 30 then label = label:sub(1, 28) .. ".." end
            disp.drawText(PLOT_X + 3, ly, label, FUNC_COLORS[slot], disp.rgb(5, 8, 15))
            ly = ly + 10
        end
    end

    -- Status bar at bottom
    disp.fillRect(0, 280, 320, 40, disp.rgb(10, 12, 20))
    disp.setFont(0)
    local info = string.format("x:[%.3g,%.3g] y:[%.3g,%.3g]",
        vp.xmin, vp.xmax, vp.ymin, vp.ymax)
    disp.drawText(4, 282, info, disp.rgb(120, 130, 150), disp.rgb(10, 12, 20))

    local help = "Arrows:pan +/-:zoom R:reset I:input T:table Enter:trace"
    disp.drawText(4, 294, help, disp.rgb(80, 90, 110), disp.rgb(10, 12, 20))

    graph.dirty = false
end

---------------------------------------------------------------------------
-- Drawing: Trace mode overlay
---------------------------------------------------------------------------

function UIGraph.draw_trace(disp, engine)
    -- Draw graph first
    UIGraph.draw_view(disp, engine)

    -- Trace crosshair
    local x_val = graph:px_to_x(PLOT_X + trace_px, PLOT_X, PLOT_W)
    local y_val = graph:eval_at(engine, trace_slot, x_val)

    local crosshair_col = disp.WHITE

    -- Vertical line at trace position
    disp.fillRect(PLOT_X + trace_px, PLOT_Y, 1, PLOT_H, disp.rgb(40, 50, 70))

    if y_val then
        local py = math.floor(graph:y_to_px(y_val, PLOT_Y, PLOT_H))
        if py >= PLOT_Y and py <= PLOT_Y + PLOT_H then
            -- Horizontal line at y
            disp.fillRect(PLOT_X, py, PLOT_W, 1, disp.rgb(40, 50, 70))
            -- Dot at intersection
            disp.fillRect(PLOT_X + trace_px - 2, py - 2, 5, 5, crosshair_col)
        end
    end

    -- Info bar
    disp.fillRect(0, 280, 320, 40, disp.rgb(10, 12, 20))
    disp.setFont(0)
    local y_str = y_val and string.format("%.8g", y_val) or "undef"
    local info = string.format("y%d: x=%.8g  y=%s", trace_slot, x_val, y_str)
    disp.drawText(4, 282, info, FUNC_COLORS[trace_slot], disp.rgb(10, 12, 20))
    disp.drawText(4, 294, "L/R:move U/D:switch func Enter/Esc:back", disp.rgb(80, 90, 110), disp.rgb(10, 12, 20))
end

---------------------------------------------------------------------------
-- Drawing: Table mode
---------------------------------------------------------------------------

function UIGraph.draw_table(disp, engine)
    init_colors(disp)

    disp.fillRect(0, 34, 320, 286, disp.rgb(5, 8, 15))
    disp.setFont(0)

    -- Header
    local header_y = 36
    local header_bg = disp.rgb(20, 25, 40)
    disp.fillRect(0, header_y, 320, 12, header_bg)
    disp.drawText(4, header_y + 2, "x", disp.rgb(180, 190, 210), header_bg)

    -- Find active slots
    local active_slots = {}
    for i = 1, 4 do
        if graph.expressions[i] ~= "" then
            active_slots[#active_slots + 1] = i
        end
    end

    local col_w = (#active_slots > 0) and math.floor(250 / #active_slots) or 250
    for ci, slot in ipairs(active_slots) do
        local cx = 70 + (ci - 1) * col_w
        disp.drawText(cx, header_y + 2, "y" .. slot, FUNC_COLORS[slot], header_bg)
    end

    -- Rows
    local row_h = 12
    local max_rows = math.floor(230 / row_h)
    local bg1 = disp.rgb(5, 8, 15)
    local bg2 = disp.rgb(12, 15, 25)

    for r = 0, max_rows - 1 do
        local x_val = table_start + (table_scroll + r) * table_step
        local ry = header_y + 14 + r * row_h
        local bg = (r % 2 == 0) and bg1 or bg2

        disp.fillRect(0, ry, 320, row_h, bg)
        disp.drawText(4, ry + 2, string.format("%.4g", x_val), disp.rgb(180, 190, 210), bg)

        for ci, slot in ipairs(active_slots) do
            local y_val = graph:eval_at(engine, slot, x_val)
            local cx = 70 + (ci - 1) * col_w
            local text = y_val and string.format("%.6g", y_val) or "--"
            disp.drawText(cx, ry + 2, text, FUNC_COLORS[slot], bg)
        end
    end

    -- Status bar
    disp.fillRect(0, 280, 320, 40, disp.rgb(10, 12, 20))
    disp.setFont(0)
    disp.drawText(4, 282, string.format("Start:%.4g  Step:%.4g", table_start + table_scroll * table_step, table_step),
        disp.rgb(120, 130, 150), disp.rgb(10, 12, 20))
    disp.drawText(4, 294, "U/D:scroll L/R:step Enter/Esc:back", disp.rgb(80, 90, 110), disp.rgb(10, 12, 20))
end

---------------------------------------------------------------------------
-- Full-screen draw dispatcher (called from main loop when in view/trace/table)
---------------------------------------------------------------------------

function UIGraph.draw_fullscreen(disp, engine)
    init_colors(disp)
    if mode == "trace" then
        UIGraph.draw_trace(disp, engine)
    elseif mode == "table" then
        UIGraph.draw_table(disp, engine)
    else
        UIGraph.draw_view(disp, engine)
    end
end

--- Returns true if graph is in a fullscreen mode (view/trace/table).
function UIGraph.is_fullscreen()
    return mode ~= "input"
end

return UIGraph
