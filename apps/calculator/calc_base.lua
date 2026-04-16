-- calc_base.lua — Number base conversion and bitwise operations
-- Pure Lua 5.4, no PicOS dependencies. Testable on host.

local Base = {}

---------------------------------------------------------------------------
-- Conversion to string (unsigned)
---------------------------------------------------------------------------

local HEX_DIGITS = "0123456789ABCDEF"

local function int_to_base(n, base)
    n = math.floor(n)
    if n == 0 then return "0" end
    local sign = ""
    if n < 0 then
        sign = "-"
        n = -n
    end
    local chars = {}
    while n > 0 do
        local d = (n % base) + 1
        chars[#chars + 1] = HEX_DIGITS:sub(d, d)
        n = math.floor(n / base)
    end
    -- Reverse
    local result = {}
    for i = #chars, 1, -1 do
        result[#result + 1] = chars[i]
    end
    return sign .. table.concat(result)
end

function Base.to_hex(n) return int_to_base(n, 16) end
function Base.to_oct(n) return int_to_base(n, 8) end
function Base.to_bin(n) return int_to_base(n, 2) end
function Base.to_dec(n) return tostring(math.floor(n)) end

---------------------------------------------------------------------------
-- Format with prefix
---------------------------------------------------------------------------

function Base.format(n, base)
    if base == 16 then return "0x" .. Base.to_hex(n)
    elseif base == 8 then return "0o" .. Base.to_oct(n)
    elseif base == 2 then return "0b" .. Base.to_bin(n)
    else return Base.to_dec(n)
    end
end

---------------------------------------------------------------------------
-- Display all bases simultaneously
---------------------------------------------------------------------------

function Base.display_all(n)
    n = math.floor(n)
    return {
        dec = Base.to_dec(n),
        hex = Base.to_hex(n),
        oct = Base.to_oct(n),
        bin = Base.to_bin(n),
    }
end

---------------------------------------------------------------------------
-- Parsing
---------------------------------------------------------------------------

function Base.parse(str, default_base)
    if not str or str == "" then return nil end
    str = str:match("^%s*(.-)%s*$")  -- trim

    -- Detect prefix
    if str:sub(1, 2) == "0x" or str:sub(1, 2) == "0X" then
        return tonumber(str:sub(3), 16)
    elseif str:sub(1, 2) == "0b" or str:sub(1, 2) == "0B" then
        return tonumber(str:sub(3), 2)
    elseif str:sub(1, 2) == "0o" or str:sub(1, 2) == "0O" then
        return tonumber(str:sub(3), 8)
    end

    -- Use default base or try decimal
    return tonumber(str, default_base or 10)
end

---------------------------------------------------------------------------
-- Bitwise operations (Lua 5.4 native)
---------------------------------------------------------------------------

function Base.band(a, b)  return a & b end
function Base.bor(a, b)   return a | b end
function Base.bxor(a, b)  return a ~ b end
function Base.lshift(a, n) return a << n end
function Base.rshift(a, n) return a >> n end

function Base.bnot(a, bits)
    bits = bits or 32
    local mask = (1 << bits) - 1
    return (~a) & mask
end

return Base
