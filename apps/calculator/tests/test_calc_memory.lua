-- test_calc_memory.lua — Tests for memory and history
-- Run with: cd apps/calculator/tests && lua test_all.lua

local lu = require("luaunit")
local Memory = dofile("../calc_memory.lua")

---------------------------------------------------------------------------
-- Memory store/recall
---------------------------------------------------------------------------

TestMemory = {}

function TestMemory:test_initial_state()
    local m = Memory.new()
    lu.assertFalse(m:has_value())
    lu.assertEquals(m:recall(), 0)
end

function TestMemory:test_store_and_recall()
    local m = Memory.new()
    m:store(42)
    lu.assertTrue(m:has_value())
    lu.assertEquals(m:recall(), 42)
end

function TestMemory:test_store_overwrites()
    local m = Memory.new()
    m:store(42)
    m:store(99)
    lu.assertEquals(m:recall(), 99)
end

function TestMemory:test_clear()
    local m = Memory.new()
    m:store(42)
    m:clear()
    lu.assertFalse(m:has_value())
    lu.assertEquals(m:recall(), 0)
end

function TestMemory:test_add()
    local m = Memory.new()
    m:store(10)
    m:add(5)
    lu.assertEquals(m:recall(), 15)
end

function TestMemory:test_add_from_empty()
    local m = Memory.new()
    m:add(7)
    lu.assertTrue(m:has_value())
    lu.assertEquals(m:recall(), 7)
end

function TestMemory:test_subtract()
    local m = Memory.new()
    m:store(10)
    m:subtract(3)
    lu.assertEquals(m:recall(), 7)
end

function TestMemory:test_subtract_from_empty()
    local m = Memory.new()
    m:subtract(5)
    lu.assertTrue(m:has_value())
    lu.assertEquals(m:recall(), -5)
end

---------------------------------------------------------------------------
-- History
---------------------------------------------------------------------------

TestHistory = {}

function TestHistory:test_empty_history()
    local m = Memory.new()
    lu.assertEquals(#m:get_history(), 0)
end

function TestHistory:test_push_and_get()
    local m = Memory.new()
    m:push_history("2+3", 5)
    m:push_history("10/2", 5)
    m:push_history("7*8", 56)
    local h = m:get_history()
    lu.assertEquals(#h, 3)
    lu.assertEquals(h[1].expr, "2+3")
    lu.assertEquals(h[1].result, 5)
    lu.assertEquals(h[3].expr, "7*8")
    lu.assertEquals(h[3].result, 56)
end

function TestHistory:test_history_cap()
    local m = Memory.new(5)  -- cap at 5
    for i = 1, 8 do
        m:push_history("expr" .. i, i)
    end
    local h = m:get_history()
    lu.assertEquals(#h, 5)
    -- Oldest entries should be dropped
    lu.assertEquals(h[1].expr, "expr4")
    lu.assertEquals(h[5].expr, "expr8")
end

function TestHistory:test_last_answer()
    local m = Memory.new()
    lu.assertNil(m:get_last_answer())
    m:push_history("2+3", 5)
    lu.assertEquals(m:get_last_answer(), 5)
    m:push_history("10*2", 20)
    lu.assertEquals(m:get_last_answer(), 20)
end

function TestHistory:test_clear_history()
    local m = Memory.new()
    m:push_history("1+1", 2)
    m:push_history("2+2", 4)
    m:clear_history()
    lu.assertEquals(#m:get_history(), 0)
    lu.assertNil(m:get_last_answer())
end

---------------------------------------------------------------------------
-- Serialization
---------------------------------------------------------------------------

TestSerialization = {}

function TestSerialization:test_serialize_empty()
    local m = Memory.new()
    local json = m:serialize()
    lu.assertNotNil(json)
    lu.assertStrContains(json, "history")
end

function TestSerialization:test_round_trip()
    local m = Memory.new()
    m:push_history("2+3", 5)
    m:push_history("sin(45)", 0.707)
    m:store(42)

    local json = m:serialize()
    local m2 = Memory.new()
    m2:deserialize(json)

    lu.assertEquals(#m2:get_history(), 2)
    lu.assertEquals(m2:get_history()[1].expr, "2+3")
    lu.assertEquals(m2:get_last_answer(), 0.707)
    -- Note: memory value (MS) is not serialized — only history
end
