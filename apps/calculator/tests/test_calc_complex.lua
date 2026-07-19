-- test_calc_complex.lua — Tests for complex number support
-- Run with: cd apps/calculator/tests && lua test_all.lua

local lu = require("luaunit")
local Engine = dofile("../calc_engine.lua")
local Functions = dofile("../calc_functions.lua")
local Complex = dofile("../calc_complex.lua")

local function make_engine(mode)
    local eng = Engine.new()
    Functions.register_all(eng)
    eng:set_angle_mode(mode or "deg")
    return eng
end

local function make_complex_engine(mode)
    local eng = make_engine(mode)
    eng:set_complex_mode(true, Complex)
    Functions.register_complex(eng, Complex)
    return eng
end

---------------------------------------------------------------------------
-- Complex type basics
---------------------------------------------------------------------------

TestComplexType = {}

function TestComplexType:test_new()
    local c = Complex.new(3, 4)
    lu.assertEquals(c.re, 3)
    lu.assertEquals(c.im, 4)
end

function TestComplexType:test_is()
    local c = Complex.new(1, 2)
    lu.assertTrue(Complex.is(c))
    lu.assertFalse(Complex.is(42))
    lu.assertFalse(Complex.is("hello"))
    lu.assertFalse(Complex.is(nil))
end

function TestComplexType:test_from_real()
    local c = Complex.from_real(5)
    lu.assertEquals(c.re, 5)
    lu.assertEquals(c.im, 0)
end

function TestComplexType:test_ensure_real()
    local c = Complex.ensure(7)
    lu.assertTrue(Complex.is(c))
    lu.assertEquals(c.re, 7)
    lu.assertEquals(c.im, 0)
end

function TestComplexType:test_ensure_complex()
    local c = Complex.new(3, 4)
    lu.assertEquals(Complex.ensure(c), c)  -- same object
end

---------------------------------------------------------------------------
-- Complex arithmetic
---------------------------------------------------------------------------

TestComplexArith = {}

function TestComplexArith:test_add()
    local r = Complex.add(Complex.new(1, 2), Complex.new(3, 4))
    lu.assertEquals(r.re, 4)
    lu.assertEquals(r.im, 6)
end

function TestComplexArith:test_sub()
    local r = Complex.sub(Complex.new(5, 3), Complex.new(2, 1))
    lu.assertEquals(r.re, 3)
    lu.assertEquals(r.im, 2)
end

function TestComplexArith:test_mul()
    -- (1+2i)(3+4i) = 3+4i+6i+8i^2 = 3+10i-8 = -5+10i
    local r = Complex.mul(Complex.new(1, 2), Complex.new(3, 4))
    lu.assertAlmostEquals(r.re, -5, 1e-10)
    lu.assertAlmostEquals(r.im, 10, 1e-10)
end

function TestComplexArith:test_div()
    -- (4+2i)/(1+i) = (4+2i)(1-i)/2 = (4-4i+2i-2i^2)/2 = (6-2i)/2 = 3-i
    local r = Complex.div(Complex.new(4, 2), Complex.new(1, 1))
    lu.assertAlmostEquals(r.re, 3, 1e-10)
    lu.assertAlmostEquals(r.im, -1, 1e-10)
end

function TestComplexArith:test_div_by_zero()
    lu.assertErrorMsgContains("Division by zero", function()
        Complex.div(Complex.new(1, 0), Complex.new(0, 0))
    end)
end

function TestComplexArith:test_neg()
    local r = Complex.neg(Complex.new(3, -4))
    lu.assertEquals(r.re, -3)
    lu.assertEquals(r.im, 4)
end

function TestComplexArith:test_mul_with_real()
    local r = Complex.mul(Complex.new(2, 3), 4)
    lu.assertAlmostEquals(r.re, 8, 1e-10)
    lu.assertAlmostEquals(r.im, 12, 1e-10)
end

---------------------------------------------------------------------------
-- Complex unary operations
---------------------------------------------------------------------------

TestComplexUnary = {}

function TestComplexUnary:test_abs_3_4()
    local r = Complex.cabs(Complex.new(3, 4))
    lu.assertAlmostEquals(r, 5, 1e-10)
end

function TestComplexUnary:test_arg_real_positive()
    local r = Complex.carg(Complex.new(1, 0))
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestComplexUnary:test_arg_pure_imaginary()
    local r = Complex.carg(Complex.new(0, 1))
    lu.assertAlmostEquals(r, math.pi / 2, 1e-10)
end

function TestComplexUnary:test_conj()
    local r = Complex.conj(Complex.new(3, 4))
    lu.assertEquals(r.re, 3)
    lu.assertEquals(r.im, -4)
end

function TestComplexUnary:test_sqrt_negative()
    -- sqrt(-1) = i
    local r = Complex.csqrt(Complex.new(-1, 0))
    lu.assertAlmostEquals(r.re, 0, 1e-10)
    lu.assertAlmostEquals(r.im, 1, 1e-10)
end

function TestComplexUnary:test_sqrt_positive()
    local r = Complex.csqrt(Complex.new(4, 0))
    lu.assertAlmostEquals(r.re, 2, 1e-10)
    lu.assertAlmostEquals(r.im, 0, 1e-10)
end

function TestComplexUnary:test_exp_pure_imaginary()
    -- e^(i*pi) = -1
    local r = Complex.cexp(Complex.new(0, math.pi))
    lu.assertAlmostEquals(r.re, -1, 1e-10)
    lu.assertAlmostEquals(r.im, 0, 1e-10)
end

function TestComplexUnary:test_log_negative()
    -- ln(-1) = i*pi
    local r = Complex.clog(Complex.new(-1, 0))
    lu.assertAlmostEquals(r.re, 0, 1e-10)
    lu.assertAlmostEquals(r.im, math.pi, 1e-10)
end

---------------------------------------------------------------------------
-- Complex trigonometric
---------------------------------------------------------------------------

TestComplexTrig = {}

function TestComplexTrig:test_sin_real()
    local r = Complex.csin(Complex.new(math.pi / 2, 0))
    lu.assertAlmostEquals(r.re, 1, 1e-10)
    lu.assertAlmostEquals(r.im, 0, 1e-10)
end

function TestComplexTrig:test_cos_real()
    local r = Complex.ccos(Complex.new(0, 0))
    lu.assertAlmostEquals(r.re, 1, 1e-10)
    lu.assertAlmostEquals(r.im, 0, 1e-10)
end

function TestComplexTrig:test_sin_imaginary()
    -- sin(i) = i*sinh(1)
    local r = Complex.csin(Complex.new(0, 1))
    lu.assertAlmostEquals(r.re, 0, 1e-10)
    local expected_im = (math.exp(1) - math.exp(-1)) / 2  -- sinh(1)
    lu.assertAlmostEquals(r.im, expected_im, 1e-10)
end

---------------------------------------------------------------------------
-- Complex formatting
---------------------------------------------------------------------------

TestComplexFormat = {}

function TestComplexFormat:test_pure_real()
    lu.assertEquals(Complex.format(Complex.new(3, 0)), "3")
end

function TestComplexFormat:test_pure_imaginary()
    lu.assertEquals(Complex.format(Complex.new(0, 2)), "2i")
end

function TestComplexFormat:test_imaginary_one()
    lu.assertEquals(Complex.format(Complex.new(0, 1)), "i")
end

function TestComplexFormat:test_imaginary_neg_one()
    lu.assertEquals(Complex.format(Complex.new(0, -1)), "-i")
end

function TestComplexFormat:test_mixed_positive()
    lu.assertEquals(Complex.format(Complex.new(3, 4)), "3+4i")
end

function TestComplexFormat:test_mixed_negative_im()
    lu.assertEquals(Complex.format(Complex.new(3, -4)), "3-4i")
end

function TestComplexFormat:test_mixed_one_im()
    lu.assertEquals(Complex.format(Complex.new(5, 1)), "5+i")
end

function TestComplexFormat:test_near_zero_snapped()
    lu.assertEquals(Complex.format(Complex.new(1e-15, 2)), "2i")
end

---------------------------------------------------------------------------
-- Engine integration with complex mode
---------------------------------------------------------------------------

TestComplexEngine = {}

function TestComplexEngine:setUp()
    self.eng = make_complex_engine("deg")
end

function TestComplexEngine:test_i_constant()
    local r = self.eng:evaluate("i")
    lu.assertTrue(Complex.is(r))
    lu.assertAlmostEquals(r.re, 0, 1e-10)
    lu.assertAlmostEquals(r.im, 1, 1e-10)
end

function TestComplexEngine:test_i_squared()
    -- i^2 = -1 (should unwrap to real since im=0)
    local r = self.eng:evaluate("i^2")
    lu.assertEquals(type(r), "number")
    lu.assertAlmostEquals(r, -1, 1e-10)
end

function TestComplexEngine:test_sqrt_negative()
    -- sqrt(-1) = i
    local r = self.eng:evaluate("sqrt(-1)")
    lu.assertTrue(Complex.is(r))
    lu.assertAlmostEquals(r.re, 0, 1e-10)
    lu.assertAlmostEquals(r.im, 1, 1e-10)
end

function TestComplexEngine:test_real_addition()
    -- Real arithmetic should still work
    local r = self.eng:evaluate("2+3")
    lu.assertEquals(type(r), "number")
    lu.assertAlmostEquals(r, 5, 1e-10)
end

function TestComplexEngine:test_complex_arithmetic()
    -- (3+4i) is expressed as 3+4*i
    local r = self.eng:evaluate("(3+4*i)*(1+2*i)")
    -- (3+4i)(1+2i) = 3+6i+4i+8i^2 = 3+10i-8 = -5+10i
    lu.assertTrue(Complex.is(r))
    lu.assertAlmostEquals(r.re, -5, 1e-10)
    lu.assertAlmostEquals(r.im, 10, 1e-10)
end

function TestComplexEngine:test_real_func()
    local r = self.eng:evaluate("real(3+4*i)")
    lu.assertEquals(type(r), "number")
    lu.assertAlmostEquals(r, 3, 1e-10)
end

function TestComplexEngine:test_imag_func()
    local r = self.eng:evaluate("imag(3+4*i)")
    lu.assertEquals(type(r), "number")
    lu.assertAlmostEquals(r, 4, 1e-10)
end

function TestComplexEngine:test_cabs_func()
    local r = self.eng:evaluate("cabs(3+4*i)")
    lu.assertEquals(type(r), "number")
    lu.assertAlmostEquals(r, 5, 1e-10)
end

function TestComplexEngine:test_conj_func()
    local r = self.eng:evaluate("conj(3+4*i)")
    lu.assertTrue(Complex.is(r))
    lu.assertAlmostEquals(r.re, 3, 1e-10)
    lu.assertAlmostEquals(r.im, -4, 1e-10)
end

function TestComplexEngine:test_format_complex()
    local r = self.eng:evaluate("sqrt(-1)")
    local s = self.eng:format_result(r)
    lu.assertEquals(s, "i")
end

---------------------------------------------------------------------------
-- Existing tests must still pass with complex_mode OFF
---------------------------------------------------------------------------

TestComplexOff = {}

function TestComplexOff:setUp()
    self.eng = make_engine("deg")  -- complex mode OFF
end

function TestComplexOff:test_basic_arithmetic()
    local r = self.eng:evaluate("2+3*4")
    lu.assertAlmostEquals(r, 14, 1e-10)
end

function TestComplexOff:test_trig()
    local r = self.eng:evaluate("sin(30)")
    lu.assertAlmostEquals(r, 0.5, 1e-10)
end

function TestComplexOff:test_sqrt_positive()
    local r = self.eng:evaluate("sqrt(4)")
    lu.assertAlmostEquals(r, 2, 1e-10)
end

function TestComplexOff:test_sqrt_negative_errors()
    local r, err = self.eng:evaluate("sqrt(-1)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

function TestComplexOff:test_no_i_constant()
    local r, err = self.eng:evaluate("i")
    lu.assertNil(r)
    lu.assertNotNil(err)
end
