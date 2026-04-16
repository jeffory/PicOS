-- test_calc_engine.lua — Tests for calculator engine (tokenizer + evaluator)
-- Run with: cd apps/calculator/tests && lua test_all.lua

local lu = require("luaunit")
local Engine = dofile("../calc_engine.lua")

---------------------------------------------------------------------------
-- Tokenizer tests
---------------------------------------------------------------------------

TestTokenizer = {}

function TestTokenizer:setUp()
    self.eng = Engine.new()
end

function TestTokenizer:test_single_integer()
    local tokens = self.eng:tokenize("42")
    lu.assertEquals(#tokens, 1)
    lu.assertEquals(tokens[1].type, "number")
    lu.assertEquals(tokens[1].value, 42)
end

function TestTokenizer:test_decimal_number()
    local tokens = self.eng:tokenize("3.14")
    lu.assertEquals(#tokens, 1)
    lu.assertEquals(tokens[1].type, "number")
    lu.assertAlmostEquals(tokens[1].value, 3.14, 1e-10)
end

function TestTokenizer:test_leading_decimal()
    local tokens = self.eng:tokenize(".5")
    lu.assertEquals(#tokens, 1)
    lu.assertEquals(tokens[1].type, "number")
    lu.assertAlmostEquals(tokens[1].value, 0.5, 1e-10)
end

function TestTokenizer:test_simple_addition()
    local tokens = self.eng:tokenize("3+4")
    lu.assertEquals(#tokens, 3)
    lu.assertEquals(tokens[1].type, "number")
    lu.assertEquals(tokens[2].type, "op")
    lu.assertEquals(tokens[2].value, "+")
    lu.assertEquals(tokens[3].type, "number")
end

function TestTokenizer:test_all_operators()
    local tokens = self.eng:tokenize("1+2-3*4/5%6^7")
    lu.assertEquals(#tokens, 13)
    lu.assertEquals(tokens[2].value, "+")
    lu.assertEquals(tokens[4].value, "-")
    lu.assertEquals(tokens[6].value, "*")
    lu.assertEquals(tokens[8].value, "/")
    lu.assertEquals(tokens[10].value, "%")
    lu.assertEquals(tokens[12].value, "^")
end

function TestTokenizer:test_parentheses()
    local tokens = self.eng:tokenize("(1+2)*3")
    lu.assertEquals(#tokens, 7)
    lu.assertEquals(tokens[1].type, "lparen")
    lu.assertEquals(tokens[5].type, "rparen")
end

function TestTokenizer:test_whitespace_ignored()
    local tokens = self.eng:tokenize("  3  +  4  ")
    lu.assertEquals(#tokens, 3)
    lu.assertEquals(tokens[1].value, 3)
    lu.assertEquals(tokens[3].value, 4)
end

function TestTokenizer:test_unary_minus_at_start()
    local tokens = self.eng:tokenize("-5")
    lu.assertEquals(#tokens, 2)
    lu.assertEquals(tokens[1].type, "unary")
    lu.assertEquals(tokens[1].value, "-")
    lu.assertEquals(tokens[2].type, "number")
    lu.assertEquals(tokens[2].value, 5)
end

function TestTokenizer:test_unary_minus_after_operator()
    local tokens = self.eng:tokenize("2*-3")
    lu.assertEquals(#tokens, 4)
    lu.assertEquals(tokens[1].type, "number")
    lu.assertEquals(tokens[2].type, "op")
    lu.assertEquals(tokens[3].type, "unary")
    lu.assertEquals(tokens[4].type, "number")
end

function TestTokenizer:test_unary_minus_after_lparen()
    local tokens = self.eng:tokenize("(-3)")
    lu.assertEquals(tokens[2].type, "unary")
end

function TestTokenizer:test_function_token()
    local tokens = self.eng:tokenize("sin(45)")
    lu.assertEquals(#tokens, 4)
    lu.assertEquals(tokens[1].type, "func")
    lu.assertEquals(tokens[1].value, "sin")
    lu.assertEquals(tokens[2].type, "lparen")
    lu.assertEquals(tokens[3].type, "number")
    lu.assertEquals(tokens[4].type, "rparen")
end

function TestTokenizer:test_constant_token()
    local tokens = self.eng:tokenize("pi")
    lu.assertEquals(#tokens, 1)
    lu.assertEquals(tokens[1].type, "const")
    lu.assertEquals(tokens[1].value, "pi")
end

function TestTokenizer:test_comma_token()
    local tokens = self.eng:tokenize("nPr(5,2)")
    lu.assertEquals(#tokens, 6)
    lu.assertEquals(tokens[4].type, "comma")
end

function TestTokenizer:test_empty_string()
    local tokens, err = self.eng:tokenize("")
    lu.assertTrue(tokens == nil or #tokens == 0)
end

function TestTokenizer:test_invalid_character()
    local tokens, err = self.eng:tokenize("3@4")
    lu.assertNil(tokens)
    lu.assertNotNil(err)
end

---------------------------------------------------------------------------
-- Evaluator tests — basic arithmetic
---------------------------------------------------------------------------

TestEvaluator = {}

function TestEvaluator:setUp()
    self.eng = Engine.new()
end

function TestEvaluator:test_single_number()
    local result, err = self.eng:evaluate("42")
    lu.assertNil(err)
    lu.assertAlmostEquals(result, 42, 1e-10)
end

function TestEvaluator:test_addition()
    local result, err = self.eng:evaluate("2+3")
    lu.assertNil(err)
    lu.assertAlmostEquals(result, 5, 1e-10)
end

function TestEvaluator:test_subtraction()
    local result, err = self.eng:evaluate("10-3")
    lu.assertNil(err)
    lu.assertAlmostEquals(result, 7, 1e-10)
end

function TestEvaluator:test_multiplication()
    local result, err = self.eng:evaluate("4*5")
    lu.assertNil(err)
    lu.assertAlmostEquals(result, 20, 1e-10)
end

function TestEvaluator:test_division()
    local result, err = self.eng:evaluate("15/4")
    lu.assertNil(err)
    lu.assertAlmostEquals(result, 3.75, 1e-10)
end

function TestEvaluator:test_modulo()
    local result, err = self.eng:evaluate("10%3")
    lu.assertNil(err)
    lu.assertAlmostEquals(result, 1, 1e-10)
end

function TestEvaluator:test_power()
    local result, err = self.eng:evaluate("2^3")
    lu.assertNil(err)
    lu.assertAlmostEquals(result, 8, 1e-10)
end

---------------------------------------------------------------------------
-- Evaluator tests — precedence and associativity
---------------------------------------------------------------------------

TestPrecedence = {}

function TestPrecedence:setUp()
    self.eng = Engine.new()
end

function TestPrecedence:test_multiply_before_add()
    local result = self.eng:evaluate("2+3*4")
    lu.assertAlmostEquals(result, 14, 1e-10)
end

function TestPrecedence:test_divide_before_subtract()
    local result = self.eng:evaluate("10-6/3")
    lu.assertAlmostEquals(result, 8, 1e-10)
end

function TestPrecedence:test_power_before_multiply()
    local result = self.eng:evaluate("2*3^2")
    lu.assertAlmostEquals(result, 18, 1e-10)
end

function TestPrecedence:test_parentheses_override()
    local result = self.eng:evaluate("(2+3)*4")
    lu.assertAlmostEquals(result, 20, 1e-10)
end

function TestPrecedence:test_nested_parentheses()
    local result = self.eng:evaluate("((2+3)*4)+1")
    lu.assertAlmostEquals(result, 21, 1e-10)
end

function TestPrecedence:test_power_right_associative()
    local result = self.eng:evaluate("2^3^2")
    lu.assertAlmostEquals(result, 512, 1e-10)  -- 2^(3^2) = 2^9 = 512
end

function TestPrecedence:test_complex_expression()
    local result = self.eng:evaluate("3+4*2/(1-5)^2")
    lu.assertAlmostEquals(result, 3.5, 1e-10)  -- 3 + (4*2)/((1-5)^2) = 3 + 8/16
end

---------------------------------------------------------------------------
-- Evaluator tests — unary minus
---------------------------------------------------------------------------

TestUnaryMinus = {}

function TestUnaryMinus:setUp()
    self.eng = Engine.new()
end

function TestUnaryMinus:test_negative_number()
    local result = self.eng:evaluate("-5")
    lu.assertAlmostEquals(result, -5, 1e-10)
end

function TestUnaryMinus:test_negative_in_expression()
    local result = self.eng:evaluate("2*-3")
    lu.assertAlmostEquals(result, -6, 1e-10)
end

function TestUnaryMinus:test_negative_in_parens()
    local result = self.eng:evaluate("(-3)+4")
    lu.assertAlmostEquals(result, 1, 1e-10)
end

function TestUnaryMinus:test_double_negative()
    local result = self.eng:evaluate("--5")
    lu.assertAlmostEquals(result, 5, 1e-10)
end

function TestUnaryMinus:test_negative_power()
    local result = self.eng:evaluate("-2^2")
    lu.assertAlmostEquals(result, -4, 1e-10)  -- -(2^2), not (-2)^2
end

---------------------------------------------------------------------------
-- Evaluator tests — error handling
---------------------------------------------------------------------------

TestErrors = {}

function TestErrors:setUp()
    self.eng = Engine.new()
end

function TestErrors:test_division_by_zero()
    local result, err = self.eng:evaluate("1/0")
    lu.assertNil(result)
    lu.assertNotNil(err)
    lu.assertStrContains(err, "ivision")  -- "Division by zero" or similar
end

function TestErrors:test_modulo_by_zero()
    local result, err = self.eng:evaluate("5%0")
    lu.assertNil(result)
    lu.assertNotNil(err)
end

function TestErrors:test_mismatched_parens_open()
    local result, err = self.eng:evaluate("1+(2*3")
    lu.assertNil(result)
    lu.assertNotNil(err)
    lu.assertStrContains(err, "arenthes")
end

function TestErrors:test_mismatched_parens_close()
    local result, err = self.eng:evaluate("1+2)*3")
    lu.assertNil(result)
    lu.assertNotNil(err)
end

function TestErrors:test_empty_expression()
    local result, err = self.eng:evaluate("")
    lu.assertNil(result)
    lu.assertNotNil(err)
end

function TestErrors:test_trailing_operator()
    local result, err = self.eng:evaluate("3+")
    lu.assertNil(result)
    lu.assertNotNil(err)
end

function TestErrors:test_consecutive_operators()
    local result, err = self.eng:evaluate("3++4")
    -- This could be interpreted as 3+(+4) or error — we'll accept either
    -- but it must not crash
    lu.assertTrue(result ~= nil or err ~= nil)
end

function TestErrors:test_unknown_function()
    local result, err = self.eng:evaluate("foo(5)")
    lu.assertNil(result)
    lu.assertNotNil(err)
    lu.assertStrContains(err, "nknown")
end

---------------------------------------------------------------------------
-- Result formatting tests
---------------------------------------------------------------------------

TestFormatResult = {}

function TestFormatResult:setUp()
    self.eng = Engine.new()
end

function TestFormatResult:test_integer_no_decimal()
    lu.assertEquals(self.eng:format_result(42), "42")
end

function TestFormatResult:test_negative_integer()
    lu.assertEquals(self.eng:format_result(-7), "-7")
end

function TestFormatResult:test_zero()
    lu.assertEquals(self.eng:format_result(0), "0")
end

function TestFormatResult:test_simple_decimal()
    lu.assertEquals(self.eng:format_result(3.14), "3.14")
end

function TestFormatResult:test_trailing_zeros_stripped()
    lu.assertEquals(self.eng:format_result(3.10), "3.1")
end

function TestFormatResult:test_large_number_scientific()
    local s = self.eng:format_result(1.23e15)
    lu.assertStrContains(s, "e")  -- Should use scientific notation
end

function TestFormatResult:test_small_number_scientific()
    local s = self.eng:format_result(1.23e-15)
    lu.assertStrContains(s, "e")
end

function TestFormatResult:test_infinity()
    local s = self.eng:format_result(math.huge)
    lu.assertStrContains(s, "nf")  -- "Infinity" or "inf"
end

function TestFormatResult:test_negative_infinity()
    local s = self.eng:format_result(-math.huge)
    lu.assertStrContains(s, "nf")
end

function TestFormatResult:test_reasonable_precision()
    -- 1/3 should have reasonable decimal places, not 20
    local s = self.eng:format_result(1/3)
    lu.assertTrue(#s <= 14)  -- At most 10 sig digits + sign + decimal + "0."
end

---------------------------------------------------------------------------
-- Integration: evaluate + format round-trip
---------------------------------------------------------------------------

TestIntegration = {}

function TestIntegration:setUp()
    self.eng = Engine.new()
end

function TestIntegration:test_simple_expression()
    local result = self.eng:evaluate("2+3")
    lu.assertEquals(self.eng:format_result(result), "5")
end

function TestIntegration:test_decimal_result()
    local result = self.eng:evaluate("10/3")
    local s = self.eng:format_result(result)
    lu.assertStrContains(s, "3.333")
end

function TestIntegration:test_large_result()
    local result = self.eng:evaluate("2^50")
    local s = self.eng:format_result(result)
    -- 2^50 = 1125899906842624 — should be in scientific or full form
    lu.assertNotNil(s)
    lu.assertTrue(#s > 0)
end

---------------------------------------------------------------------------
-- Zero-arity function support
---------------------------------------------------------------------------

TestZeroArity = {}

function TestZeroArity:setUp()
    self.eng = Engine.new()
    self.eng:register_function("fortytwo", 0, function()
        return 42
    end)
    self.eng:register_function("add1", 1, function(x)
        return x + 1
    end)
end

function TestZeroArity:test_zero_arity_basic()
    local r = self.eng:evaluate("fortytwo()")
    lu.assertAlmostEquals(r, 42, 1e-10)
end

function TestZeroArity:test_zero_arity_in_expression()
    local r = self.eng:evaluate("fortytwo()+8")
    lu.assertAlmostEquals(r, 50, 1e-10)
end

function TestZeroArity:test_zero_arity_nested()
    local r = self.eng:evaluate("add1(fortytwo())")
    lu.assertAlmostEquals(r, 43, 1e-10)
end

function TestZeroArity:test_zero_arity_multiplied()
    local r = self.eng:evaluate("2*fortytwo()")
    lu.assertAlmostEquals(r, 84, 1e-10)
end
