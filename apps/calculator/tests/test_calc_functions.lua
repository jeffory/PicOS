-- test_calc_functions.lua — Tests for scientific functions
-- Run with: cd apps/calculator/tests && lua test_all.lua

local lu = require("luaunit")
local Engine = dofile("../calc_engine.lua")
local Functions = dofile("../calc_functions.lua")

local function make_engine(mode)
    local eng = Engine.new()
    Functions.register_all(eng)
    eng:set_angle_mode(mode or "deg")
    return eng
end

---------------------------------------------------------------------------
-- Trigonometric functions — degrees mode
---------------------------------------------------------------------------

TestTrigDeg = {}

function TestTrigDeg:setUp()
    self.eng = make_engine("deg")
end

function TestTrigDeg:test_sin_0()
    local r = self.eng:evaluate("sin(0)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestTrigDeg:test_sin_30()
    local r = self.eng:evaluate("sin(30)")
    lu.assertAlmostEquals(r, 0.5, 1e-10)
end

function TestTrigDeg:test_sin_90()
    local r = self.eng:evaluate("sin(90)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestTrigDeg:test_cos_0()
    local r = self.eng:evaluate("cos(0)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestTrigDeg:test_cos_90()
    local r = self.eng:evaluate("cos(90)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestTrigDeg:test_tan_45()
    local r = self.eng:evaluate("tan(45)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestTrigDeg:test_asin_1()
    local r = self.eng:evaluate("asin(1)")
    lu.assertAlmostEquals(r, 90, 1e-10)
end

function TestTrigDeg:test_acos_0()
    local r = self.eng:evaluate("acos(0)")
    lu.assertAlmostEquals(r, 90, 1e-10)
end

function TestTrigDeg:test_atan_1()
    local r = self.eng:evaluate("atan(1)")
    lu.assertAlmostEquals(r, 45, 1e-10)
end

---------------------------------------------------------------------------
-- Trigonometric functions — radians mode
---------------------------------------------------------------------------

TestTrigRad = {}

function TestTrigRad:setUp()
    self.eng = make_engine("rad")
end

function TestTrigRad:test_sin_pi_over_2()
    local r = self.eng:evaluate("sin(pi/2)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestTrigRad:test_cos_pi()
    local r = self.eng:evaluate("cos(pi)")
    lu.assertAlmostEquals(r, -1, 1e-10)
end

function TestTrigRad:test_sin_0()
    local r = self.eng:evaluate("sin(0)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestTrigRad:test_asin_returns_radians()
    local r = self.eng:evaluate("asin(1)")
    lu.assertAlmostEquals(r, math.pi / 2, 1e-10)
end

---------------------------------------------------------------------------
-- Trigonometric functions — gradians mode
---------------------------------------------------------------------------

TestTrigGrad = {}

function TestTrigGrad:setUp()
    self.eng = make_engine("grad")
end

function TestTrigGrad:test_sin_100()
    local r = self.eng:evaluate("sin(100)")
    lu.assertAlmostEquals(r, 1, 1e-10)  -- 100 grad = 90 deg
end

function TestTrigGrad:test_cos_200()
    local r = self.eng:evaluate("cos(200)")
    lu.assertAlmostEquals(r, -1, 1e-10)  -- 200 grad = 180 deg
end

function TestTrigGrad:test_asin_returns_gradians()
    local r = self.eng:evaluate("asin(1)")
    lu.assertAlmostEquals(r, 100, 1e-10)  -- 90 deg = 100 grad
end

---------------------------------------------------------------------------
-- Logarithmic functions
---------------------------------------------------------------------------

TestLogs = {}

function TestLogs:setUp()
    self.eng = make_engine("deg")
end

function TestLogs:test_ln_1()
    local r = self.eng:evaluate("ln(1)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestLogs:test_ln_e()
    local r = self.eng:evaluate("ln(e)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestLogs:test_log_100()
    local r = self.eng:evaluate("log(100)")
    lu.assertAlmostEquals(r, 2, 1e-10)
end

function TestLogs:test_log_1000()
    local r = self.eng:evaluate("log(1000)")
    lu.assertAlmostEquals(r, 3, 1e-10)
end

function TestLogs:test_log2_8()
    local r = self.eng:evaluate("log2(8)")
    lu.assertAlmostEquals(r, 3, 1e-10)
end

function TestLogs:test_log2_1()
    local r = self.eng:evaluate("log2(1)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

---------------------------------------------------------------------------
-- Logarithm domain errors
---------------------------------------------------------------------------

TestLogErrors = {}

function TestLogErrors:setUp()
    self.eng = make_engine("deg")
end

function TestLogErrors:test_ln_0()
    local r, err = self.eng:evaluate("ln(0)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

function TestLogErrors:test_ln_negative()
    local r, err = self.eng:evaluate("ln(-1)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

function TestLogErrors:test_log_0()
    local r, err = self.eng:evaluate("log(0)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

function TestLogErrors:test_sqrt_negative()
    local r, err = self.eng:evaluate("sqrt(-1)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

function TestLogErrors:test_asin_out_of_range()
    local r, err = self.eng:evaluate("asin(2)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

function TestLogErrors:test_acos_out_of_range()
    local r, err = self.eng:evaluate("acos(-2)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

---------------------------------------------------------------------------
-- Exponential and power functions
---------------------------------------------------------------------------

TestExpPow = {}

function TestExpPow:setUp()
    self.eng = make_engine("deg")
end

function TestExpPow:test_exp_0()
    local r = self.eng:evaluate("exp(0)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestExpPow:test_exp_1()
    local r = self.eng:evaluate("exp(1)")
    lu.assertAlmostEquals(r, math.exp(1), 1e-10)
end

function TestExpPow:test_sqrt_4()
    local r = self.eng:evaluate("sqrt(4)")
    lu.assertAlmostEquals(r, 2, 1e-10)
end

function TestExpPow:test_sqrt_2()
    local r = self.eng:evaluate("sqrt(2)")
    lu.assertAlmostEquals(r, math.sqrt(2), 1e-10)
end

function TestExpPow:test_cbrt_27()
    local r = self.eng:evaluate("cbrt(27)")
    lu.assertAlmostEquals(r, 3, 1e-10)
end

function TestExpPow:test_cbrt_negative()
    local r = self.eng:evaluate("cbrt(-8)")
    lu.assertAlmostEquals(r, -2, 1e-10)
end

function TestExpPow:test_sqr_5()
    local r = self.eng:evaluate("sqr(5)")
    lu.assertAlmostEquals(r, 25, 1e-10)
end

function TestExpPow:test_cube_3()
    local r = self.eng:evaluate("cube(3)")
    lu.assertAlmostEquals(r, 27, 1e-10)
end

function TestExpPow:test_abs_negative()
    local r = self.eng:evaluate("abs(-7)")
    lu.assertAlmostEquals(r, 7, 1e-10)
end

function TestExpPow:test_abs_positive()
    local r = self.eng:evaluate("abs(3)")
    lu.assertAlmostEquals(r, 3, 1e-10)
end

function TestExpPow:test_recip()
    local r = self.eng:evaluate("recip(4)")
    lu.assertAlmostEquals(r, 0.25, 1e-10)
end

function TestExpPow:test_recip_zero()
    local r, err = self.eng:evaluate("recip(0)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

---------------------------------------------------------------------------
-- Constants in expressions
---------------------------------------------------------------------------

TestConstants = {}

function TestConstants:setUp()
    self.eng = make_engine("rad")
end

function TestConstants:test_pi_value()
    local r = self.eng:evaluate("pi")
    lu.assertAlmostEquals(r, math.pi, 1e-10)
end

function TestConstants:test_e_value()
    local r = self.eng:evaluate("e")
    lu.assertAlmostEquals(r, math.exp(1), 1e-10)
end

function TestConstants:test_2_times_pi()
    local r = self.eng:evaluate("2*pi")
    lu.assertAlmostEquals(r, 2 * math.pi, 1e-10)
end

function TestConstants:test_e_squared()
    local r = self.eng:evaluate("e^2")
    lu.assertAlmostEquals(r, math.exp(2), 1e-10)
end

---------------------------------------------------------------------------
-- Compound expressions
---------------------------------------------------------------------------

TestCompound = {}

function TestCompound:setUp()
    self.eng = make_engine("deg")
end

function TestCompound:test_pythagorean_identity()
    local r = self.eng:evaluate("sin(45)^2+cos(45)^2")
    lu.assertAlmostEquals(r, 1.0, 1e-10)
end

function TestCompound:test_log_power()
    local r = self.eng:evaluate("log(10^5)")
    lu.assertAlmostEquals(r, 5, 1e-10)
end

function TestCompound:test_nested_functions()
    local r = self.eng:evaluate("sqrt(abs(-16))")
    lu.assertAlmostEquals(r, 4, 1e-10)
end

function TestCompound:test_exp_ln_roundtrip()
    local r = self.eng:evaluate("exp(ln(42))")
    lu.assertAlmostEquals(r, 42, 1e-8)
end

---------------------------------------------------------------------------
-- Advanced math: factorial, permutations, combinations (Stage 3)
---------------------------------------------------------------------------

TestFactorial = {}

function TestFactorial:setUp()
    self.eng = make_engine("deg")
end

function TestFactorial:test_fact_0()
    local r = self.eng:evaluate("fact(0)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestFactorial:test_fact_1()
    local r = self.eng:evaluate("fact(1)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestFactorial:test_fact_5()
    local r = self.eng:evaluate("fact(5)")
    lu.assertAlmostEquals(r, 120, 1e-10)
end

function TestFactorial:test_fact_10()
    local r = self.eng:evaluate("fact(10)")
    lu.assertAlmostEquals(r, 3628800, 1e-10)
end

function TestFactorial:test_fact_20()
    local r = self.eng:evaluate("fact(20)")
    lu.assertAlmostEquals(r, 2432902008176640000, 1e3)  -- large, allow tolerance
end

function TestFactorial:test_fact_negative()
    local r, err = self.eng:evaluate("fact(-1)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

function TestFactorial:test_fact_non_integer()
    local r, err = self.eng:evaluate("fact(1.5)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end

---------------------------------------------------------------------------
-- Permutations and combinations
---------------------------------------------------------------------------

TestCombinatorics = {}

function TestCombinatorics:setUp()
    self.eng = make_engine("deg")
end

function TestCombinatorics:test_nPr_5_2()
    local r = self.eng:evaluate("nPr(5,2)")
    lu.assertAlmostEquals(r, 20, 1e-10)
end

function TestCombinatorics:test_nPr_10_3()
    local r = self.eng:evaluate("nPr(10,3)")
    lu.assertAlmostEquals(r, 720, 1e-10)
end

function TestCombinatorics:test_nCr_5_2()
    local r = self.eng:evaluate("nCr(5,2)")
    lu.assertAlmostEquals(r, 10, 1e-10)
end

function TestCombinatorics:test_nCr_10_3()
    local r = self.eng:evaluate("nCr(10,3)")
    lu.assertAlmostEquals(r, 120, 1e-10)
end

function TestCombinatorics:test_nCr_5_0()
    local r = self.eng:evaluate("nCr(5,0)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestCombinatorics:test_nCr_5_5()
    local r = self.eng:evaluate("nCr(5,5)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

---------------------------------------------------------------------------
-- Hyperbolic functions
---------------------------------------------------------------------------

TestHyperbolic = {}

function TestHyperbolic:setUp()
    self.eng = make_engine("deg")
end

function TestHyperbolic:test_sinh_0()
    local r = self.eng:evaluate("sinh(0)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestHyperbolic:test_cosh_0()
    local r = self.eng:evaluate("cosh(0)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestHyperbolic:test_tanh_0()
    local r = self.eng:evaluate("tanh(0)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestHyperbolic:test_sinh_1()
    local r = self.eng:evaluate("sinh(1)")
    local expected = (math.exp(1) - math.exp(-1)) / 2
    lu.assertAlmostEquals(r, expected, 1e-10)
end

function TestHyperbolic:test_cosh_1()
    local r = self.eng:evaluate("cosh(1)")
    local expected = (math.exp(1) + math.exp(-1)) / 2
    lu.assertAlmostEquals(r, expected, 1e-10)
end

function TestHyperbolic:test_asinh_0()
    local r = self.eng:evaluate("asinh(0)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestHyperbolic:test_acosh_1()
    local r = self.eng:evaluate("acosh(1)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestHyperbolic:test_atanh_0()
    local r = self.eng:evaluate("atanh(0)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

---------------------------------------------------------------------------
-- Rounding functions
---------------------------------------------------------------------------

TestRounding = {}

function TestRounding:setUp()
    self.eng = make_engine("deg")
end

function TestRounding:test_floor_positive()
    local r = self.eng:evaluate("floor(3.7)")
    lu.assertAlmostEquals(r, 3, 1e-10)
end

function TestRounding:test_floor_negative()
    local r = self.eng:evaluate("floor(-3.2)")
    lu.assertAlmostEquals(r, -4, 1e-10)
end

function TestRounding:test_ceil_positive()
    local r = self.eng:evaluate("ceil(3.2)")
    lu.assertAlmostEquals(r, 4, 1e-10)
end

function TestRounding:test_ceil_negative()
    local r = self.eng:evaluate("ceil(-3.7)")
    lu.assertAlmostEquals(r, -3, 1e-10)
end

function TestRounding:test_round_up()
    local r = self.eng:evaluate("round(3.5)")
    lu.assertAlmostEquals(r, 4, 1e-10)
end

function TestRounding:test_round_down()
    local r = self.eng:evaluate("round(3.4)")
    lu.assertAlmostEquals(r, 3, 1e-10)
end

function TestRounding:test_round_negative()
    local r = self.eng:evaluate("round(-3.5)")
    lu.assertAlmostEquals(r, -4, 1e-10)
end

---------------------------------------------------------------------------
-- Number theory: gcd, lcm
---------------------------------------------------------------------------

TestNumberTheory = {}

function TestNumberTheory:setUp()
    self.eng = make_engine("deg")
end

function TestNumberTheory:test_gcd_12_8()
    local r = self.eng:evaluate("gcd(12,8)")
    lu.assertAlmostEquals(r, 4, 1e-10)
end

function TestNumberTheory:test_gcd_7_13()
    local r = self.eng:evaluate("gcd(7,13)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestNumberTheory:test_gcd_0_5()
    local r = self.eng:evaluate("gcd(0,5)")
    lu.assertAlmostEquals(r, 5, 1e-10)
end

function TestNumberTheory:test_gcd_negative()
    local r = self.eng:evaluate("gcd(-12,8)")
    lu.assertAlmostEquals(r, 4, 1e-10)
end

function TestNumberTheory:test_lcm_4_6()
    local r = self.eng:evaluate("lcm(4,6)")
    lu.assertAlmostEquals(r, 12, 1e-10)
end

function TestNumberTheory:test_lcm_3_7()
    local r = self.eng:evaluate("lcm(3,7)")
    lu.assertAlmostEquals(r, 21, 1e-10)
end

function TestNumberTheory:test_lcm_0_5()
    local r = self.eng:evaluate("lcm(0,5)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

---------------------------------------------------------------------------
-- atan2
---------------------------------------------------------------------------

TestAtan2 = {}

function TestAtan2:setUp()
    self.eng = make_engine("deg")
end

function TestAtan2:test_atan2_1_1()
    local r = self.eng:evaluate("atan2(1,1)")
    lu.assertAlmostEquals(r, 45, 1e-10)
end

function TestAtan2:test_atan2_0_1()
    local r = self.eng:evaluate("atan2(0,1)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestAtan2:test_atan2_1_0()
    local r = self.eng:evaluate("atan2(1,0)")
    lu.assertAlmostEquals(r, 90, 1e-10)
end

function TestAtan2:test_atan2_rad_mode()
    self.eng:set_angle_mode("rad")
    local r = self.eng:evaluate("atan2(1,1)")
    lu.assertAlmostEquals(r, math.pi / 4, 1e-10)
end

---------------------------------------------------------------------------
-- Random functions
---------------------------------------------------------------------------

TestRandom = {}

function TestRandom:setUp()
    self.eng = make_engine("deg")
end

function TestRandom:test_rand_in_range()
    local r = self.eng:evaluate("rand()")
    lu.assertNotNil(r)
    lu.assertTrue(r >= 0 and r < 1)
end

function TestRandom:test_randint_in_range()
    local r = self.eng:evaluate("randint(1,6)")
    lu.assertNotNil(r)
    lu.assertTrue(r >= 1 and r <= 6)
    lu.assertEquals(r, math.floor(r))  -- must be integer
end

function TestRandom:test_randint_reversed_args()
    local r = self.eng:evaluate("randint(6,1)")
    lu.assertNotNil(r)
    lu.assertTrue(r >= 1 and r <= 6)
end

---------------------------------------------------------------------------
-- Part extraction: frac, int
---------------------------------------------------------------------------

TestParts = {}

function TestParts:setUp()
    self.eng = make_engine("deg")
end

function TestParts:test_frac_positive()
    local r = self.eng:evaluate("frac(3.7)")
    lu.assertAlmostEquals(r, 0.7, 1e-10)
end

function TestParts:test_frac_integer()
    local r = self.eng:evaluate("frac(5)")
    lu.assertAlmostEquals(r, 0, 1e-10)
end

function TestParts:test_int_positive()
    local r = self.eng:evaluate("int(3.7)")
    lu.assertAlmostEquals(r, 3, 1e-10)
end

function TestParts:test_int_negative()
    local r = self.eng:evaluate("int(-3.7)")
    lu.assertAlmostEquals(r, -3, 1e-10)
end

---------------------------------------------------------------------------
-- Min, max, mod
---------------------------------------------------------------------------

TestMinMaxMod = {}

function TestMinMaxMod:setUp()
    self.eng = make_engine("deg")
end

function TestMinMaxMod:test_max_3_5()
    local r = self.eng:evaluate("max(3,5)")
    lu.assertAlmostEquals(r, 5, 1e-10)
end

function TestMinMaxMod:test_min_3_5()
    local r = self.eng:evaluate("min(3,5)")
    lu.assertAlmostEquals(r, 3, 1e-10)
end

function TestMinMaxMod:test_max_negative()
    local r = self.eng:evaluate("max(-10,5)")
    lu.assertAlmostEquals(r, 5, 1e-10)
end

function TestMinMaxMod:test_min_negative()
    local r = self.eng:evaluate("min(-10,5)")
    lu.assertAlmostEquals(r, -10, 1e-10)
end

function TestMinMaxMod:test_mod_10_3()
    local r = self.eng:evaluate("mod(10,3)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestMinMaxMod:test_mod_7_2()
    local r = self.eng:evaluate("mod(7,2)")
    lu.assertAlmostEquals(r, 1, 1e-10)
end

function TestMinMaxMod:test_mod_div_zero()
    local r, err = self.eng:evaluate("mod(5,0)")
    lu.assertNil(r)
    lu.assertNotNil(err)
end
