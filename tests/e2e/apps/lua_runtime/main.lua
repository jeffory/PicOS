-- E2E fixture for the Lua VM configuration (LUA_32BITS, LUAI_MAXSTACK=1000,
-- coroutine/utf8 availability, text-only load). Every probe logs one
-- "LR <NAME> <value>" line; the test asserts on the exact values.

local log = picocalc.sys.log
local sys = picocalc.sys

local function probe(name, fn)
    local ok, v = pcall(fn)
    log("LR " .. name .. " " .. (ok and tostring(v) or ("ERR " .. tostring(v))))
end

-- 1 ── numeric representation
probe("MAXINT", function() return math.maxinteger end)
probe("MININT", function() return math.mininteger end)
probe("FLOATTYPE", function() return math.type(1.5) end)
probe("HUGE", function() return 1e39 == math.huge end)
probe("FMT01", function() return string.format("%.10g", 0.1) end)
probe("WRAP", function() return math.maxinteger + 1 == math.mininteger end)
-- hex literals wrap modulo 2^32; %x prints the unsigned 32-bit pattern
probe("HEX", function() return string.format("%08x", 0x811C9DC5 * 16777619) end)

-- JSON floats: shortest text that decodes back to the same float
probe("JSONF", function()
    local third = 1 / 3
    local enc = picocalc.json.encode({ 0.1, third })
    local dec = picocalc.json.decode(enc)
    return enc .. " rt=" .. tostring(dec[1] == 0.1 and dec[2] == third)
end)

-- 2 ── stdlib: coroutine and utf8 are open; io/os/package/debug are not
probe("CORO", function()
    local gen = coroutine.wrap(function()
        for i = 1, 3 do coroutine.yield(i) end
    end)
    return gen() + gen() + gen()
end)
probe("UTF8", function()
    local s = utf8.char(72, 0x20AC)
    return #s .. ":" .. utf8.len(s)
end)
probe("SANDBOX", function()
    return tostring(io) .. "," .. tostring(os) .. "," .. tostring(package)
        .. "," .. tostring(debug)
end)

-- 3 ── load is text-only
local function bc_of(f) return string.dump(f) end
local dumped = bc_of(function() return 42 end)
probe("LOADTEXT", function() return load("return 6 * 7")() end)
probe("LOADBC", function()
    local f, err = load(dumped)
    return tostring(f) .. " " .. tostring(err)
end)
probe("LOADBC_MODE_B", function()
    local f, err = load(dumped, "=bc", "b")
    return tostring(f) .. " " .. tostring(err)
end)
probe("LOADBC_READER", function()
    local sent = false
    local f, err = load(function()
        if sent then return nil end
        sent = true
        return dumped
    end)
    return tostring(f) .. " " .. tostring(err)
end)
probe("LOADBC_HEADER", function()
    local f, err = load("\27Lua\x54\0")
    return tostring(f) .. " " .. tostring(err)
end)

-- 4 ── re-baseline numbers (logged, not asserted). Runs before the
--      recursion probe so the grown-then-abandoned stack does not skew GC.
local t0 = sys.getTimeMs()
local acc = 0.0
for i = 1, 2000000 do acc = acc + (i % 7) * 0.5 end
log("LR BENCH_LOOP_MS " .. (sys.getTimeMs() - t0) .. " acc=" .. tostring(acc))
collectgarbage("collect")
local base_kb = collectgarbage("count")
local t = {}
for i = 1, 20000 do t[i] = { i, i * 0.5, "s" .. i } end
log(string.format("LR BENCH_GC_KB base=%.1f with_tables=%.1f", base_kb,
    collectgarbage("count")))
t = nil
collectgarbage("collect")

-- 5 ── unbounded recursion raises a Lua error instead of hanging/faulting
local depth = 0
local function recurse()
    depth = depth + 1
    return 1 + recurse()  -- not a tail call
end
local rok, rerr = pcall(recurse)
log("LR RECURSE ok=" .. tostring(rok) .. " overflow="
    .. tostring(type(rerr) == "string" and rerr:find("stack overflow") ~= nil))
log("LR DEPTH " .. depth)

log("LR DONE")
