-- calc_complex.lua — Complex number type and arithmetic
-- Pure Lua 5.4, no PicOS dependencies. Testable on host.

local Complex = {}
Complex.__index = Complex

function Complex.new(re, im)
    return setmetatable({re = re or 0, im = im or 0}, Complex)
end

function Complex.is(v)
    return type(v) == "table" and getmetatable(v) == Complex
end

function Complex.from_real(n)
    return Complex.new(n, 0)
end

--- Promote a value to Complex if it isn't already.
function Complex.ensure(v)
    if Complex.is(v) then return v end
    return Complex.from_real(v)
end

---------------------------------------------------------------------------
-- Arithmetic
---------------------------------------------------------------------------

function Complex.add(a, b)
    a, b = Complex.ensure(a), Complex.ensure(b)
    return Complex.new(a.re + b.re, a.im + b.im)
end

function Complex.sub(a, b)
    a, b = Complex.ensure(a), Complex.ensure(b)
    return Complex.new(a.re - b.re, a.im - b.im)
end

function Complex.mul(a, b)
    a, b = Complex.ensure(a), Complex.ensure(b)
    return Complex.new(
        a.re * b.re - a.im * b.im,
        a.re * b.im + a.im * b.re
    )
end

function Complex.div(a, b)
    a, b = Complex.ensure(a), Complex.ensure(b)
    local denom = b.re * b.re + b.im * b.im
    if denom == 0 then error("Division by zero") end
    return Complex.new(
        (a.re * b.re + a.im * b.im) / denom,
        (a.im * b.re - a.re * b.im) / denom
    )
end

function Complex.neg(a)
    a = Complex.ensure(a)
    return Complex.new(-a.re, -a.im)
end

---------------------------------------------------------------------------
-- Unary operations returning real numbers
---------------------------------------------------------------------------

function Complex.cabs(a)
    a = Complex.ensure(a)
    return math.sqrt(a.re * a.re + a.im * a.im)
end

function Complex.carg(a)
    a = Complex.ensure(a)
    return math.atan(a.im, a.re)
end

---------------------------------------------------------------------------
-- Unary operations returning Complex
---------------------------------------------------------------------------

function Complex.conj(a)
    a = Complex.ensure(a)
    return Complex.new(a.re, -a.im)
end

function Complex.csqrt(a)
    a = Complex.ensure(a)
    local r = math.sqrt(a.re * a.re + a.im * a.im)
    if r == 0 then return Complex.new(0, 0) end
    -- Principal square root
    local re = math.sqrt((r + a.re) / 2)
    local im = math.sqrt((r - a.re) / 2)
    if a.im < 0 then im = -im end
    return Complex.new(re, im)
end

function Complex.cexp(a)
    a = Complex.ensure(a)
    local er = math.exp(a.re)
    return Complex.new(er * math.cos(a.im), er * math.sin(a.im))
end

function Complex.clog(a)
    a = Complex.ensure(a)
    local r = math.sqrt(a.re * a.re + a.im * a.im)
    if r == 0 then error("log: domain error (complex zero)") end
    return Complex.new(math.log(r), math.atan(a.im, a.re))
end

function Complex.pow(a, b)
    a, b = Complex.ensure(a), Complex.ensure(b)
    -- a^b = exp(b * log(a))
    if a.re == 0 and a.im == 0 then
        if b.re > 0 then return Complex.new(0, 0) end
        error("0^b: domain error for non-positive exponent")
    end
    return Complex.cexp(Complex.mul(b, Complex.clog(a)))
end

---------------------------------------------------------------------------
-- Complex trigonometric
---------------------------------------------------------------------------

function Complex.csin(a)
    a = Complex.ensure(a)
    -- sin(z) = (exp(iz) - exp(-iz)) / (2i)
    local iz = Complex.new(-a.im, a.re)  -- i*z
    local niz = Complex.new(a.im, -a.re) -- -i*z
    local eiz = Complex.cexp(iz)
    local eniz = Complex.cexp(niz)
    local diff = Complex.sub(eiz, eniz)
    return Complex.new(diff.im / 2, -diff.re / 2) -- diff / (2i)
end

function Complex.ccos(a)
    a = Complex.ensure(a)
    -- cos(z) = (exp(iz) + exp(-iz)) / 2
    local iz = Complex.new(-a.im, a.re)
    local niz = Complex.new(a.im, -a.re)
    local eiz = Complex.cexp(iz)
    local eniz = Complex.cexp(niz)
    local sum = Complex.add(eiz, eniz)
    return Complex.new(sum.re / 2, sum.im / 2)
end

function Complex.ctan(a)
    return Complex.div(Complex.csin(a), Complex.ccos(a))
end

---------------------------------------------------------------------------
-- Extraction
---------------------------------------------------------------------------

function Complex.real(a)
    a = Complex.ensure(a)
    return a.re
end

function Complex.imag(a)
    a = Complex.ensure(a)
    return a.im
end

---------------------------------------------------------------------------
-- Conversion
---------------------------------------------------------------------------

function Complex.to_polar(a)
    a = Complex.ensure(a)
    return Complex.cabs(a), Complex.carg(a)
end

function Complex.from_polar(r, theta)
    return Complex.new(r * math.cos(theta), r * math.sin(theta))
end

---------------------------------------------------------------------------
-- Display formatting
---------------------------------------------------------------------------

local function fmt_num(n)
    if n == math.floor(n) and math.abs(n) < 1e15 then
        return string.format("%.0f", n)
    end
    return string.format("%.10g", n)
end

function Complex.format(a)
    a = Complex.ensure(a)
    local re, im = a.re, a.im

    -- Snap near-zero values
    if math.abs(re) < 1e-14 then re = 0 end
    if math.abs(im) < 1e-14 then im = 0 end

    if im == 0 then
        return fmt_num(re)
    elseif re == 0 then
        if im == 1 then return "i"
        elseif im == -1 then return "-i"
        else return fmt_num(im) .. "i"
        end
    else
        if im == 1 then return fmt_num(re) .. "+i"
        elseif im == -1 then return fmt_num(re) .. "-i"
        elseif im > 0 then return fmt_num(re) .. "+" .. fmt_num(im) .. "i"
        else return fmt_num(re) .. fmt_num(im) .. "i"
        end
    end
end

return Complex
