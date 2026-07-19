-- test_calc_base.lua — Tests for number base conversion and bitwise ops
-- Run with: cd apps/calculator/tests && lua test_all.lua

local lu = require("luaunit")
local Base = dofile("../calc_base.lua")

---------------------------------------------------------------------------
-- Conversion to string
---------------------------------------------------------------------------

TestBaseConvert = {}

function TestBaseConvert:test_to_hex_255()
    lu.assertEquals(Base.to_hex(255), "FF")
end

function TestBaseConvert:test_to_hex_0()
    lu.assertEquals(Base.to_hex(0), "0")
end

function TestBaseConvert:test_to_hex_16()
    lu.assertEquals(Base.to_hex(16), "10")
end

function TestBaseConvert:test_to_bin_10()
    lu.assertEquals(Base.to_bin(10), "1010")
end

function TestBaseConvert:test_to_bin_0()
    lu.assertEquals(Base.to_bin(0), "0")
end

function TestBaseConvert:test_to_bin_255()
    lu.assertEquals(Base.to_bin(255), "11111111")
end

function TestBaseConvert:test_to_oct_8()
    lu.assertEquals(Base.to_oct(8), "10")
end

function TestBaseConvert:test_to_oct_0()
    lu.assertEquals(Base.to_oct(0), "0")
end

function TestBaseConvert:test_to_oct_255()
    lu.assertEquals(Base.to_oct(255), "377")
end

function TestBaseConvert:test_to_dec_42()
    lu.assertEquals(Base.to_dec(42), "42")
end

---------------------------------------------------------------------------
-- Format with prefix
---------------------------------------------------------------------------

TestBaseFormat = {}

function TestBaseFormat:test_format_hex()
    lu.assertEquals(Base.format(255, 16), "0xFF")
end

function TestBaseFormat:test_format_bin()
    lu.assertEquals(Base.format(10, 2), "0b1010")
end

function TestBaseFormat:test_format_oct()
    lu.assertEquals(Base.format(8, 8), "0o10")
end

function TestBaseFormat:test_format_dec()
    lu.assertEquals(Base.format(42, 10), "42")
end

---------------------------------------------------------------------------
-- Parsing
---------------------------------------------------------------------------

TestBaseParse = {}

function TestBaseParse:test_parse_hex_prefix()
    lu.assertEquals(Base.parse("0xFF"), 255)
end

function TestBaseParse:test_parse_hex_upper()
    lu.assertEquals(Base.parse("0XFF"), 255)
end

function TestBaseParse:test_parse_bin_prefix()
    lu.assertEquals(Base.parse("0b1010"), 10)
end

function TestBaseParse:test_parse_oct_prefix()
    lu.assertEquals(Base.parse("0o17"), 15)
end

function TestBaseParse:test_parse_dec()
    lu.assertEquals(Base.parse("42"), 42)
end

function TestBaseParse:test_parse_hex_no_prefix()
    lu.assertEquals(Base.parse("FF", 16), 255)
end

function TestBaseParse:test_parse_bin_no_prefix()
    lu.assertEquals(Base.parse("1010", 2), 10)
end

function TestBaseParse:test_parse_invalid()
    local r = Base.parse("xyz")
    lu.assertNil(r)
end

---------------------------------------------------------------------------
-- Display all bases
---------------------------------------------------------------------------

TestBaseDisplayAll = {}

function TestBaseDisplayAll:test_display_all()
    local t = Base.display_all(255)
    lu.assertEquals(t.dec, "255")
    lu.assertEquals(t.hex, "FF")
    lu.assertEquals(t.oct, "377")
    lu.assertEquals(t.bin, "11111111")
end

function TestBaseDisplayAll:test_display_all_zero()
    local t = Base.display_all(0)
    lu.assertEquals(t.dec, "0")
    lu.assertEquals(t.hex, "0")
    lu.assertEquals(t.oct, "0")
    lu.assertEquals(t.bin, "0")
end

---------------------------------------------------------------------------
-- Bitwise operations
---------------------------------------------------------------------------

TestBitwise = {}

function TestBitwise:test_band()
    lu.assertEquals(Base.band(0xF0, 0x0F), 0)
end

function TestBitwise:test_band_same()
    lu.assertEquals(Base.band(0xFF, 0xFF), 0xFF)
end

function TestBitwise:test_bor()
    lu.assertEquals(Base.bor(0xF0, 0x0F), 0xFF)
end

function TestBitwise:test_bxor()
    lu.assertEquals(Base.bxor(0xFF, 0x0F), 0xF0)
end

function TestBitwise:test_bnot_8bit()
    lu.assertEquals(Base.bnot(0x00, 8), 0xFF)
end

function TestBitwise:test_bnot_16bit()
    lu.assertEquals(Base.bnot(0x00, 16), 0xFFFF)
end

function TestBitwise:test_lshift()
    lu.assertEquals(Base.lshift(1, 8), 256)
end

function TestBitwise:test_rshift()
    lu.assertEquals(Base.rshift(256, 8), 1)
end

function TestBitwise:test_lshift_0()
    lu.assertEquals(Base.lshift(42, 0), 42)
end

function TestBitwise:test_rshift_0()
    lu.assertEquals(Base.rshift(42, 0), 42)
end
