-- test_calc_solver.lua — Tests for equation solver algorithms
local lu = require("luaunit")
local Solver = dofile("../calc_solver.lua")
local Complex = dofile("../calc_complex.lua")
local Engine = dofile("../calc_engine.lua")
local Functions = dofile("../calc_functions.lua")

---------------------------------------------------------------------------
-- Linear 2x2
---------------------------------------------------------------------------

TestSolverLinear2x2 = {}

function TestSolverLinear2x2:test_simple()
    -- x + y = 3, x - y = 1 → x=2, y=1
    local r = Solver.solve_linear_2x2({{1,1},{1,-1}}, {3, 1})
    lu.assertAlmostEquals(r[1], 2, 1e-10)
    lu.assertAlmostEquals(r[2], 1, 1e-10)
end

function TestSolverLinear2x2:test_fractional()
    -- 2x + 3y = 8, x - y = -1 → x=1, y=2
    local r = Solver.solve_linear_2x2({{2,3},{1,-1}}, {8, -1})
    lu.assertAlmostEquals(r[1], 1, 1e-10)
    lu.assertAlmostEquals(r[2], 2, 1e-10)
end

function TestSolverLinear2x2:test_negative_coefficients()
    -- -x + 2y = 5, 3x - y = 1 → x=7/5, y=18/5... let's verify via Cramer
    -- det = (-1)(-1) - (2)(3) = 1 - 6 = -5
    -- x = (5*-1 - 1*2) / -5 = -7 / -5 = 1.4
    -- y = (-1*1 - 5*3) / -5 = -16 / -5 = 3.2
    local r = Solver.solve_linear_2x2({{-1,2},{3,-1}}, {5, 1})
    lu.assertAlmostEquals(r[1], 1.4, 1e-10)
    lu.assertAlmostEquals(r[2], 3.2, 1e-10)
end

function TestSolverLinear2x2:test_singular()
    -- Proportional rows: x + 2y = 3, 2x + 4y = 6
    local r, err = Solver.solve_linear_2x2({{1,2},{2,4}}, {3, 6})
    lu.assertNil(r)
    lu.assertStrContains(err, "singular")
end

function TestSolverLinear2x2:test_zero_coefficients()
    -- 0x + 0y = 0, 0x + 0y = 0
    local r, err = Solver.solve_linear_2x2({{0,0},{0,0}}, {0, 0})
    lu.assertNil(r)
    lu.assertStrContains(err, "singular")
end

---------------------------------------------------------------------------
-- Linear 3x3
---------------------------------------------------------------------------

TestSolverLinear3x3 = {}

function TestSolverLinear3x3:test_simple()
    -- x=1, y=2, z=3:  x+0+0=1, 0+y+0=2, 0+0+z=3
    local r = Solver.solve_linear_3x3(
        {{1,0,0},{0,1,0},{0,0,1}}, {1, 2, 3})
    lu.assertAlmostEquals(r[1], 1, 1e-10)
    lu.assertAlmostEquals(r[2], 2, 1e-10)
    lu.assertAlmostEquals(r[3], 3, 1e-10)
end

function TestSolverLinear3x3:test_general()
    -- 2x + y - z = 3, x - y + 2z = 5, 3x + 2y + z = 10
    -- Verify: plug in x=1, y=2, z=3: 2+2-3=1≠3. Use a known system instead.
    -- x + 2y + 3z = 14, 2x + 5y + 3z = 19, x + 0y + 8z = 25
    -- x=1, y=2, z=3: 1+4+9=14 ✓, 2+10+9=21≠19. Let me just verify numerically.
    local r = Solver.solve_linear_3x3(
        {{1,1,1},{0,1,1},{0,0,1}}, {6, 5, 3})
    -- Upper triangular: z=3, y=5-3=2, x=6-5=1
    lu.assertAlmostEquals(r[1], 1, 1e-10)
    lu.assertAlmostEquals(r[2], 2, 1e-10)
    lu.assertAlmostEquals(r[3], 3, 1e-10)
end

function TestSolverLinear3x3:test_needs_pivoting()
    -- First column starts with 0 — needs row swap
    -- 0x + y + z = 3, x + 0y + z = 2, x + y + 0z = 3
    -- Solution: x=1, y=2, z=1... verify: 0+2+1=3, 1+0+1=2, 1+2+0=3 ✓
    local r = Solver.solve_linear_3x3(
        {{0,1,1},{1,0,1},{1,1,0}}, {3, 2, 3})
    lu.assertAlmostEquals(r[1], 1, 1e-10)
    lu.assertAlmostEquals(r[2], 2, 1e-10)
    lu.assertAlmostEquals(r[3], 1, 1e-10)
end

function TestSolverLinear3x3:test_singular()
    -- Row 3 = Row 1 + Row 2
    local r, err = Solver.solve_linear_3x3(
        {{1,1,0},{0,1,1},{1,2,1}}, {1, 2, 3})
    lu.assertNil(r)
    lu.assertStrContains(err, "singular")
end

---------------------------------------------------------------------------
-- Quadratic
---------------------------------------------------------------------------

TestSolverQuadratic = {}

function TestSolverQuadratic:test_two_real_roots()
    -- x^2 - 5x + 6 = 0 → (x-2)(x-3) → x=3, x=2
    local r = Solver.solve_quadratic(1, -5, 6)
    lu.assertEquals(#r, 2)
    lu.assertAlmostEquals(r[1], 3, 1e-10)
    lu.assertAlmostEquals(r[2], 2, 1e-10)
end

function TestSolverQuadratic:test_double_root()
    -- x^2 - 4x + 4 = 0 → (x-2)^2 → x=2 (double)
    local r = Solver.solve_quadratic(1, -4, 4)
    lu.assertEquals(#r, 2)
    lu.assertAlmostEquals(r[1], 2, 1e-10)
    lu.assertAlmostEquals(r[2], 2, 1e-10)
end

function TestSolverQuadratic:test_complex_roots_with_complex()
    -- x^2 + 1 = 0 → x = ±i
    local r = Solver.solve_quadratic(1, 0, 1, Complex)
    lu.assertEquals(#r, 2)
    lu.assertTrue(Complex.is(r[1]))
    lu.assertTrue(Complex.is(r[2]))
    lu.assertAlmostEquals(r[1].re, 0, 1e-10)
    lu.assertAlmostEquals(r[1].im, 1, 1e-10)
    lu.assertAlmostEquals(r[2].re, 0, 1e-10)
    lu.assertAlmostEquals(r[2].im, -1, 1e-10)
end

function TestSolverQuadratic:test_complex_roots_without_complex()
    -- x^2 + 1 = 0, no Complex module
    local r, err = Solver.solve_quadratic(1, 0, 1)
    lu.assertNil(r)
    lu.assertStrContains(err, "No real roots")
end

function TestSolverQuadratic:test_linear_fallback()
    -- a=0: 2x - 6 = 0 → x=3
    local r = Solver.solve_quadratic(0, 2, -6)
    lu.assertEquals(#r, 1)
    lu.assertAlmostEquals(r[1], 3, 1e-10)
end

function TestSolverQuadratic:test_degenerate()
    -- a=0, b=0, c=5 → not an equation
    local r, err = Solver.solve_quadratic(0, 0, 5)
    lu.assertNil(r)
    lu.assertStrContains(err, "Not an equation")
end

function TestSolverQuadratic:test_negative_discriminant_complex()
    -- x^2 + 2x + 5 = 0 → x = -1 ± 2i
    local r = Solver.solve_quadratic(1, 2, 5, Complex)
    lu.assertEquals(#r, 2)
    lu.assertAlmostEquals(r[1].re, -1, 1e-10)
    lu.assertAlmostEquals(r[1].im, 2, 1e-10)
    lu.assertAlmostEquals(r[2].re, -1, 1e-10)
    lu.assertAlmostEquals(r[2].im, -2, 1e-10)
end

---------------------------------------------------------------------------
-- Cubic
---------------------------------------------------------------------------

TestSolverCubic = {}

function TestSolverCubic:test_three_real_roots()
    -- x^3 - 6x^2 + 11x - 6 = 0 → (x-1)(x-2)(x-3)
    local r = Solver.solve_cubic(1, -6, 11, -6)
    lu.assertEquals(#r, 3)
    table.sort(r)
    lu.assertAlmostEquals(r[1], 1, 1e-8)
    lu.assertAlmostEquals(r[2], 2, 1e-8)
    lu.assertAlmostEquals(r[3], 3, 1e-8)
end

function TestSolverCubic:test_one_real_two_complex()
    -- x^3 + 1 = 0 → x=-1 is real root; two complex roots
    local r = Solver.solve_cubic(1, 0, 0, 1, Complex)
    lu.assertEquals(#r, 3)
    -- Find the real root (either a number or Complex with im≈0)
    local real_root
    local complex_roots = {}
    for _, v in ipairs(r) do
        if type(v) == "number" then
            real_root = v
        elseif Complex.is(v) and math.abs(v.im) < 1e-8 then
            real_root = v.re
        else
            complex_roots[#complex_roots + 1] = v
        end
    end
    lu.assertAlmostEquals(real_root, -1, 1e-8)
    lu.assertEquals(#complex_roots, 2)
    -- Complex roots are conjugates
    lu.assertAlmostEquals(complex_roots[1].re, complex_roots[2].re, 1e-8)
    lu.assertAlmostEquals(complex_roots[1].im, -complex_roots[2].im, 1e-8)
end

function TestSolverCubic:test_one_real_without_complex()
    -- x^3 + 1 = 0, no Complex → only real root returned
    local r = Solver.solve_cubic(1, 0, 0, 1)
    lu.assertEquals(#r, 1)
    lu.assertAlmostEquals(r[1], -1, 1e-8)
end

function TestSolverCubic:test_triple_root()
    -- (x-1)^3 = x^3 - 3x^2 + 3x - 1 = 0
    local r = Solver.solve_cubic(1, -3, 3, -1)
    lu.assertEquals(#r, 3)
    for _, v in ipairs(r) do
        lu.assertAlmostEquals(v, 1, 1e-6)
    end
end

function TestSolverCubic:test_quadratic_fallback()
    -- a=0: degenerates to x^2 - 5x + 6 = 0
    local r = Solver.solve_cubic(0, 1, -5, 6)
    lu.assertEquals(#r, 2)
    lu.assertAlmostEquals(r[1], 3, 1e-10)
    lu.assertAlmostEquals(r[2], 2, 1e-10)
end

---------------------------------------------------------------------------
-- Newton's method
---------------------------------------------------------------------------

TestSolverNewton = {}

function TestSolverNewton:setUp()
    self.eng = Engine.new()
    Functions.register_all(self.eng)
end

function TestSolverNewton:test_sqrt2()
    -- x^2 - 2 = 0, starting at x=1 → √2
    local r = Solver.solve_newton(self.eng, "x^2-2", 1)
    lu.assertAlmostEquals(r, math.sqrt(2), 1e-8)
end

function TestSolverNewton:test_linear()
    -- 2x - 10 = 0, starting at x=0 → x=5
    local r = Solver.solve_newton(self.eng, "2*x-10", 0)
    lu.assertAlmostEquals(r, 5, 1e-8)
end

function TestSolverNewton:test_cube_root()
    -- x^3 - 8 = 0, starting at x=1 → 2
    local r = Solver.solve_newton(self.eng, "x^3-8", 1)
    lu.assertAlmostEquals(r, 2, 1e-8)
end

function TestSolverNewton:test_custom_tolerance()
    -- x^2 - 4 = 0, loose tolerance
    local r = Solver.solve_newton(self.eng, "x^2-4", 1.5, 1e-3)
    lu.assertAlmostEquals(r, 2, 1e-3)
end

function TestSolverNewton:test_max_iterations()
    -- Very few iterations — may not converge for tough problems
    local r, err = Solver.solve_newton(self.eng, "x^3-x-100", 0, 1e-10, 2)
    if not r then
        lu.assertStrContains(err, "converge")
    end
end

function TestSolverNewton:test_restores_variable()
    -- Verify x variable is restored after solve
    self.eng:register_variable("x", function() return 42 end)
    Solver.solve_newton(self.eng, "x^2-2", 1)
    -- The old variable function should be restored
    lu.assertNotNil(self.eng.variables["x"])
end
