-- test_calc_graph.lua — Tests for graph data model and coordinate math
local lu = require("luaunit")
local Graph = dofile("../calc_graph.lua")
local Engine = dofile("../calc_engine.lua")
local Functions = dofile("../calc_functions.lua")

---------------------------------------------------------------------------
-- Coordinate mapping
---------------------------------------------------------------------------

TestGraphCoords = {}

function TestGraphCoords:setUp()
    self.g = Graph.new()
    -- Default viewport: -10..10 on both axes
end

function TestGraphCoords:test_x_to_px_center()
    -- x=0 should map to center of plot
    local px = self.g:x_to_px(0, 20, 280)
    lu.assertAlmostEquals(px, 20 + 140, 1e-10)  -- 160
end

function TestGraphCoords:test_x_to_px_left()
    local px = self.g:x_to_px(-10, 20, 280)
    lu.assertAlmostEquals(px, 20, 1e-10)
end

function TestGraphCoords:test_x_to_px_right()
    local px = self.g:x_to_px(10, 20, 280)
    lu.assertAlmostEquals(px, 300, 1e-10)
end

function TestGraphCoords:test_y_to_px_center()
    -- y=0 should map to center of plot
    local py = self.g:y_to_px(0, 40, 220)
    lu.assertAlmostEquals(py, 40 + 110, 1e-10)  -- 150
end

function TestGraphCoords:test_y_to_px_top()
    -- y=10 (max) maps to plot top
    local py = self.g:y_to_px(10, 40, 220)
    lu.assertAlmostEquals(py, 40, 1e-10)
end

function TestGraphCoords:test_y_to_px_bottom()
    -- y=-10 (min) maps to plot bottom
    local py = self.g:y_to_px(-10, 40, 220)
    lu.assertAlmostEquals(py, 260, 1e-10)
end

function TestGraphCoords:test_roundtrip_x()
    local x_orig = 3.7
    local px = self.g:x_to_px(x_orig, 20, 280)
    local x_back = self.g:px_to_x(px, 20, 280)
    lu.assertAlmostEquals(x_back, x_orig, 1e-10)
end

function TestGraphCoords:test_roundtrip_y()
    local y_orig = -2.5
    local py = self.g:y_to_px(y_orig, 40, 220)
    local y_back = self.g:px_to_y(py, 40, 220)
    lu.assertAlmostEquals(y_back, y_orig, 1e-10)
end

---------------------------------------------------------------------------
-- Viewport manipulation
---------------------------------------------------------------------------

TestGraphViewport = {}

function TestGraphViewport:setUp()
    self.g = Graph.new()
end

function TestGraphViewport:test_pan_right()
    self.g:pan(0.1, 0)
    local vp = self.g.viewport
    lu.assertAlmostEquals(vp.xmin, -8, 1e-10)
    lu.assertAlmostEquals(vp.xmax, 12, 1e-10)
    lu.assertAlmostEquals(vp.ymin, -10, 1e-10)
    lu.assertAlmostEquals(vp.ymax, 10, 1e-10)
end

function TestGraphViewport:test_pan_up()
    self.g:pan(0, 0.1)
    local vp = self.g.viewport
    lu.assertAlmostEquals(vp.ymin, -8, 1e-10)
    lu.assertAlmostEquals(vp.ymax, 12, 1e-10)
end

function TestGraphViewport:test_zoom_in()
    self.g:zoom(0.5)
    local vp = self.g.viewport
    lu.assertAlmostEquals(vp.xmin, -5, 1e-10)
    lu.assertAlmostEquals(vp.xmax, 5, 1e-10)
    lu.assertAlmostEquals(vp.ymin, -5, 1e-10)
    lu.assertAlmostEquals(vp.ymax, 5, 1e-10)
end

function TestGraphViewport:test_zoom_out()
    self.g:zoom(2)
    local vp = self.g.viewport
    lu.assertAlmostEquals(vp.xmin, -20, 1e-10)
    lu.assertAlmostEquals(vp.xmax, 20, 1e-10)
end

function TestGraphViewport:test_reset()
    self.g:pan(0.5, 0.5)
    self.g:zoom(3)
    self.g:reset()
    local vp = self.g.viewport
    lu.assertAlmostEquals(vp.xmin, -10, 1e-10)
    lu.assertAlmostEquals(vp.xmax, 10, 1e-10)
    lu.assertAlmostEquals(vp.ymin, -10, 1e-10)
    lu.assertAlmostEquals(vp.ymax, 10, 1e-10)
end

function TestGraphViewport:test_invalidates_cache()
    self.g.dirty = false
    self.g.cache[1] = {1, 2, 3}
    self.g:pan(0.1, 0)
    lu.assertTrue(self.g.dirty)
    lu.assertEquals(#self.g.cache, 0)
end

---------------------------------------------------------------------------
-- Evaluation
---------------------------------------------------------------------------

TestGraphEval = {}

function TestGraphEval:setUp()
    self.g = Graph.new()
    self.eng = Engine.new()
    Functions.register_all(self.eng)
end

function TestGraphEval:test_eval_linear()
    self.g.expressions[1] = "2*x+1"
    local y = self.g:eval_at(self.eng, 1, 3)
    lu.assertAlmostEquals(y, 7, 1e-10)
end

function TestGraphEval:test_eval_quadratic()
    self.g.expressions[1] = "x^2"
    local y = self.g:eval_at(self.eng, 1, 4)
    lu.assertAlmostEquals(y, 16, 1e-10)
end

function TestGraphEval:test_eval_trig()
    self.g.expressions[2] = "sin(x)"
    self.eng:set_angle_mode("rad")
    local y = self.g:eval_at(self.eng, 2, 0)
    lu.assertAlmostEquals(y, 0, 1e-10)
end

function TestGraphEval:test_eval_empty()
    -- Empty expression returns nil
    local y = self.g:eval_at(self.eng, 1, 0)
    lu.assertNil(y)
end

function TestGraphEval:test_eval_error()
    self.g.expressions[1] = "1/0"
    local y = self.g:eval_at(self.eng, 1, 0)
    -- Division by zero may return inf → nil
    lu.assertNil(y)
end

function TestGraphEval:test_restores_x_variable()
    self.eng:register_variable("x", function() return 42 end)
    self.g.expressions[1] = "x+1"
    self.g:eval_at(self.eng, 1, 5)
    -- x variable should be restored
    lu.assertNotNil(self.eng.variables["x"])
end

---------------------------------------------------------------------------
-- Nice tick spacing
---------------------------------------------------------------------------

TestGraphTick = {}

function TestGraphTick:test_range_20()
    -- Range 20 → rough=20/6≈3.3 → pow10=1 → frac=3.3 → nice=2 → tick=2
    local tick = Graph.nice_tick(20)
    lu.assertEquals(tick, 2)
end

function TestGraphTick:test_range_1()
    -- Range 1 → rough=1/6≈0.167 → pow10=0.1 → frac=1.67 → nice=2 → tick=0.2
    local tick = Graph.nice_tick(1)
    lu.assertAlmostEquals(tick, 0.2, 1e-10)
end

function TestGraphTick:test_range_100()
    -- Range 100 → rough=100/6≈16.7 → pow10=10 → frac=1.67 → nice=2 → tick=20
    local tick = Graph.nice_tick(100)
    lu.assertEquals(tick, 20)
end

function TestGraphTick:test_range_zero()
    local tick = Graph.nice_tick(0)
    lu.assertEquals(tick, 1)
end

function TestGraphTick:test_range_small()
    -- Range 0.01 → rough≈0.00167 → pow10=0.001 → frac=1.67 → nice=2 → tick=0.002
    local tick = Graph.nice_tick(0.01)
    lu.assertAlmostEquals(tick, 0.002, 1e-10)
end
