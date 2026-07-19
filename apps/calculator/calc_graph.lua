-- calc_graph.lua — Graph data model and coordinate math
-- Pure Lua 5.4, no PicOS dependencies. Testable on host.

local Graph = {}
Graph.__index = Graph

function Graph.new()
    local self = setmetatable({}, Graph)
    self.expressions = {"", "", "", ""}   -- y1..y4
    self.active_slot = 1
    self.viewport = {xmin = -10, xmax = 10, ymin = -10, ymax = 10}
    self.dirty = true
    self.cache = {}  -- [slot] = {y_values} indexed by pixel column
    return self
end

---------------------------------------------------------------------------
-- Coordinate mapping
---------------------------------------------------------------------------

function Graph:x_to_px(x, plot_x, plot_w)
    local vp = self.viewport
    return plot_x + (x - vp.xmin) / (vp.xmax - vp.xmin) * plot_w
end

function Graph:y_to_px(y, plot_y, plot_h)
    local vp = self.viewport
    -- y increases upward, pixels increase downward
    return plot_y + (1 - (y - vp.ymin) / (vp.ymax - vp.ymin)) * plot_h
end

function Graph:px_to_x(px, plot_x, plot_w)
    local vp = self.viewport
    return vp.xmin + (px - plot_x) / plot_w * (vp.xmax - vp.xmin)
end

function Graph:px_to_y(py, plot_y, plot_h)
    local vp = self.viewport
    return vp.ymax - (py - plot_y) / plot_h * (vp.ymax - vp.ymin)
end

---------------------------------------------------------------------------
-- Evaluation
---------------------------------------------------------------------------

--- Evaluate expression in slot at a given x value.
-- Returns y (number) or nil on error.
function Graph:eval_at(engine, slot, x)
    local expr = self.expressions[slot]
    if not expr or expr == "" then return nil end

    local old_var = engine.variables["x"]
    engine:register_variable("x", function() return x end)
    local val, err = engine:evaluate(expr)
    engine.variables["x"] = old_var

    if not val then return nil end
    if type(val) ~= "number" then return nil end
    if val ~= val then return nil end  -- NaN
    if val == math.huge or val == -math.huge then return nil end
    return val
end

--- Evaluate all points for a slot across pixel columns.
-- Returns table of {px_col = y_value or nil}.
function Graph:eval_cached(engine, slot, plot_x, plot_w)
    if not self.dirty and self.cache[slot] then
        return self.cache[slot]
    end
    local points = {}
    for px = 0, plot_w - 1 do
        local x = self:px_to_x(plot_x + px, plot_x, plot_w)
        points[px] = self:eval_at(engine, slot, x)
    end
    self.cache[slot] = points
    return points
end

--- Invalidate cache (call when viewport or expressions change).
function Graph:invalidate()
    self.cache = {}
    self.dirty = true
end

---------------------------------------------------------------------------
-- Viewport manipulation
---------------------------------------------------------------------------

function Graph:pan(dx_frac, dy_frac)
    local vp = self.viewport
    local dx = (vp.xmax - vp.xmin) * dx_frac
    local dy = (vp.ymax - vp.ymin) * dy_frac
    vp.xmin = vp.xmin + dx
    vp.xmax = vp.xmax + dx
    vp.ymin = vp.ymin + dy
    vp.ymax = vp.ymax + dy
    self:invalidate()
end

function Graph:zoom(factor)
    local vp = self.viewport
    local cx = (vp.xmin + vp.xmax) / 2
    local cy = (vp.ymin + vp.ymax) / 2
    local hw = (vp.xmax - vp.xmin) / 2 * factor
    local hh = (vp.ymax - vp.ymin) / 2 * factor
    vp.xmin = cx - hw
    vp.xmax = cx + hw
    vp.ymin = cy - hh
    vp.ymax = cy + hh
    self:invalidate()
end

function Graph:reset()
    self.viewport = {xmin = -10, xmax = 10, ymin = -10, ymax = 10}
    self:invalidate()
end

---------------------------------------------------------------------------
-- Nice tick spacing
---------------------------------------------------------------------------

--- Compute a "nice" tick interval for axis labels.
-- Returns tick spacing such that there are roughly 5-10 ticks in range.
function Graph.nice_tick(range)
    if range <= 0 then return 1 end
    local rough = range / 6
    local pow10 = 10 ^ math.floor(math.log(rough, 10))
    local frac = rough / pow10
    local nice
    if frac <= 1.5 then
        nice = 1
    elseif frac <= 3.5 then
        nice = 2
    elseif frac <= 7.5 then
        nice = 5
    else
        nice = 10
    end
    return nice * pow10
end

return Graph
