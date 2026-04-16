-- calc_functions.lua — Scientific function registry
-- Pure Lua 5.4, no PicOS dependencies. Testable on host.

local Functions = {}

-- Angle conversion helpers
local function deg_to_rad(x) return x * math.pi / 180 end
local function rad_to_deg(x) return x * 180 / math.pi end
local function grad_to_rad(x) return x * math.pi / 200 end
local function rad_to_grad(x) return x * 200 / math.pi end

local function angle_to_rad(engine, x)
    local m = engine.angle_mode
    if m == "rad" then return x end
    if m == "grad" then return grad_to_rad(x) end
    return deg_to_rad(x)  -- default: deg
end

local function rad_to_angle(engine, x)
    local m = engine.angle_mode
    if m == "rad" then return x end
    if m == "grad" then return rad_to_grad(x) end
    return rad_to_deg(x)  -- default: deg
end

-- Domain-checked helpers
local function check_positive(x, name)
    if x <= 0 then error(name .. ": domain error (requires x > 0)") end
end

local function check_non_negative(x, name)
    if x < 0 then error(name .. ": domain error (requires x >= 0)") end
end

local function check_range(x, lo, hi, name)
    if x < lo or x > hi then
        error(name .. ": domain error (requires " .. lo .. " <= x <= " .. hi .. ")")
    end
end

-- Factorial (iterative, capped at 170 to avoid inf)
local function factorial(n)
    if n ~= math.floor(n) or n < 0 then
        error("fact: requires non-negative integer")
    end
    if n > 170 then
        error("fact: argument too large (max 170)")
    end
    local result = 1
    for i = 2, n do
        result = result * i
    end
    return result
end

-- Permutations: P(n,k) = n!/(n-k)!
local function permutation(n, k)
    if n ~= math.floor(n) or k ~= math.floor(k) or n < 0 or k < 0 then
        error("nPr: requires non-negative integers")
    end
    if k > n then error("nPr: k must be <= n") end
    local result = 1
    for i = n - k + 1, n do
        result = result * i
    end
    return result
end

-- Combinations: C(n,k) = n!/(k!(n-k)!)
local function combination(n, k)
    if n ~= math.floor(n) or k ~= math.floor(k) or n < 0 or k < 0 then
        error("nCr: requires non-negative integers")
    end
    if k > n then error("nCr: k must be <= n") end
    if k > n - k then k = n - k end  -- optimization: C(n,k) = C(n,n-k)
    local result = 1
    for i = 0, k - 1 do
        result = result * (n - i) / (i + 1)
    end
    return result
end

function Functions.register_all(engine)
    local eng = engine

    -- Trigonometric (angle-mode-aware)
    eng:register_function("sin", 1, function(x)
        return math.sin(angle_to_rad(eng, x))
    end)
    eng:register_function("cos", 1, function(x)
        return math.cos(angle_to_rad(eng, x))
    end)
    eng:register_function("tan", 1, function(x)
        return math.tan(angle_to_rad(eng, x))
    end)

    -- Inverse trig (returns in current angle mode)
    eng:register_function("asin", 1, function(x)
        check_range(x, -1, 1, "asin")
        return rad_to_angle(eng, math.asin(x))
    end)
    eng:register_function("acos", 1, function(x)
        check_range(x, -1, 1, "acos")
        return rad_to_angle(eng, math.acos(x))
    end)
    eng:register_function("atan", 1, function(x)
        return rad_to_angle(eng, math.atan(x))
    end)

    -- Logarithmic
    eng:register_function("ln", 1, function(x)
        check_positive(x, "ln")
        return math.log(x)
    end)
    eng:register_function("log", 1, function(x)
        check_positive(x, "log")
        return math.log(x, 10)
    end)
    eng:register_function("log2", 1, function(x)
        check_positive(x, "log2")
        return math.log(x, 2)
    end)

    -- Exponential
    eng:register_function("exp", 1, function(x) return math.exp(x) end)

    -- Power/root
    eng:register_function("sqrt", 1, function(x)
        check_non_negative(x, "sqrt")
        return math.sqrt(x)
    end)
    eng:register_function("cbrt", 1, function(x)
        if x < 0 then
            return -((-x) ^ (1/3))
        end
        return x ^ (1/3)
    end)
    eng:register_function("sqr", 1, function(x) return x * x end)
    eng:register_function("cube", 1, function(x) return x * x * x end)

    -- Misc
    eng:register_function("abs", 1, function(x) return math.abs(x) end)
    eng:register_function("recip", 1, function(x)
        if x == 0 then error("recip: division by zero") end
        return 1 / x
    end)

    -- Hyperbolic
    eng:register_function("sinh", 1, function(x)
        return (math.exp(x) - math.exp(-x)) / 2
    end)
    eng:register_function("cosh", 1, function(x)
        return (math.exp(x) + math.exp(-x)) / 2
    end)
    eng:register_function("tanh", 1, function(x)
        local ex = math.exp(2 * x)
        return (ex - 1) / (ex + 1)
    end)
    eng:register_function("asinh", 1, function(x)
        return math.log(x + math.sqrt(x * x + 1))
    end)
    eng:register_function("acosh", 1, function(x)
        if x < 1 then error("acosh: domain error (requires x >= 1)") end
        return math.log(x + math.sqrt(x * x - 1))
    end)
    eng:register_function("atanh", 1, function(x)
        if math.abs(x) >= 1 then error("atanh: domain error (requires |x| < 1)") end
        return 0.5 * math.log((1 + x) / (1 - x))
    end)

    -- Factorial and combinatorics
    eng:register_function("fact", 1, function(x) return factorial(x) end)
    eng:register_function("nPr", 2, function(n, k) return permutation(n, k) end)
    eng:register_function("nCr", 2, function(n, k) return combination(n, k) end)

    -- Rounding
    eng:register_function("floor", 1, function(x) return math.floor(x) end)
    eng:register_function("ceil", 1, function(x) return math.ceil(x) end)
    eng:register_function("round", 1, function(x)
        if x >= 0 then
            return math.floor(x + 0.5)
        else
            return math.ceil(x - 0.5)
        end
    end)

    -- Number theory
    eng:register_function("gcd", 2, function(a, b)
        a, b = math.abs(math.floor(a)), math.abs(math.floor(b))
        while b ~= 0 do a, b = b, a % b end
        return a
    end)
    eng:register_function("lcm", 2, function(a, b)
        a, b = math.abs(math.floor(a)), math.abs(math.floor(b))
        if a == 0 and b == 0 then return 0 end
        local g = a
        local tb = b
        while tb ~= 0 do g, tb = tb, g % tb end
        return (a / g) * b
    end)

    -- Two-argument arctangent (angle-mode aware)
    eng:register_function("atan2", 2, function(y, x)
        return rad_to_angle(eng, math.atan(y, x))
    end)

    -- Random
    eng:register_function("rand", 0, function()
        return math.random()
    end)
    eng:register_function("randint", 2, function(a, b)
        a, b = math.floor(a), math.floor(b)
        if a > b then a, b = b, a end
        return math.random(a, b)
    end)

    -- Part extraction
    eng:register_function("frac", 1, function(x)
        return x - math.floor(x)
    end)
    eng:register_function("int", 1, function(x)
        if x >= 0 then return math.floor(x) else return math.ceil(x) end
    end)

    -- Explicit modulo function
    eng:register_function("mod", 2, function(a, b)
        if b == 0 then error("mod: division by zero") end
        return a % b
    end)

    -- Min/max
    eng:register_function("max", 2, function(a, b) return math.max(a, b) end)
    eng:register_function("min", 2, function(a, b) return math.min(a, b) end)

    -- Constants (already in engine, but ensure phi is there)
    eng:register_constant("phi", (1 + math.sqrt(5)) / 2)
end

--- Register complex-mode function overrides and new complex-specific functions.
-- Called when complex mode is toggled on.
function Functions.register_complex(engine, Complex)
    local eng = engine

    -- Complex-specific functions (always new, not overrides)
    eng:register_function("real", 1, function(z)
        return Complex.real(z)
    end)
    eng:register_function("imag", 1, function(z)
        return Complex.imag(z)
    end)
    eng:register_function("conj", 1, function(z)
        return Complex.conj(z)
    end)
    eng:register_function("cabs", 1, function(z)
        return Complex.cabs(z)
    end)
    eng:register_function("carg", 1, function(z)
        return rad_to_angle(eng, Complex.carg(z))
    end)

    -- Complex-mode overrides (use complex_functions so they only apply in complex mode)
    eng:register_complex_function("abs", 1, function(z)
        return Complex.cabs(z)
    end)
    eng:register_complex_function("sqrt", 1, function(z)
        return Complex.csqrt(z)
    end)
    eng:register_complex_function("exp", 1, function(z)
        return Complex.cexp(z)
    end)
    eng:register_complex_function("ln", 1, function(z)
        return Complex.clog(z)
    end)
    eng:register_complex_function("log", 1, function(z)
        local result = Complex.clog(z)
        return Complex.div(result, Complex.from_real(math.log(10)))
    end)
    eng:register_complex_function("sin", 1, function(z)
        -- Convert from angle mode to radians first
        z = Complex.ensure(z)
        local zr = Complex.new(angle_to_rad(eng, z.re), z.im)
        return Complex.csin(zr)
    end)
    eng:register_complex_function("cos", 1, function(z)
        z = Complex.ensure(z)
        local zr = Complex.new(angle_to_rad(eng, z.re), z.im)
        return Complex.ccos(zr)
    end)
    eng:register_complex_function("tan", 1, function(z)
        z = Complex.ensure(z)
        local zr = Complex.new(angle_to_rad(eng, z.re), z.im)
        return Complex.ctan(zr)
    end)

    -- Register i constant
    eng:register_constant("i", Complex.new(0, 1))
end

--- Unregister complex-specific functions when complex mode is turned off.
function Functions.unregister_complex(engine)
    -- Remove complex-only functions
    engine.functions["real"] = nil
    engine.functions["imag"] = nil
    engine.functions["conj"] = nil
    engine.functions["cabs"] = nil
    engine.functions["carg"] = nil
    -- Clear complex overrides
    engine.complex_functions = {}
    -- Remove i constant
    engine.constants["i"] = nil
end

return Functions
