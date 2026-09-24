-- E2E fixture for the Lua VM configuration (LUA_32BITS, LUAI_MAXSTACK=1000,
-- LUAI_MAXCCALLS=60, coroutine/utf8 availability, text-only load). Every
-- probe logs one
-- "LR <NAME> <value>" line; the test asserts on the exact values.

local sys = picocalc.sys
-- Every line also goes to /data/com.test.luaruntime/lr.txt so a hardware run
-- (where serial capture drops [APP] lines) can be diffed against the sim.
local lines = {}
local function log(s)
    lines[#lines + 1] = s
    sys.log(s)
end

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

-- 1b ── number formatting. tostring, string.format's float conversions and
--       the JSON encoder must print identically on the device (pico_printf)
--       and in the simulator (glibc). Values are wrapped in |...| so leading
--       and trailing spaces survive the log.
local function fprobe(name, fn)
    local ok, v = pcall(fn)
    log("LR F_" .. name .. " |" .. (ok and tostring(v) or ("ERR " .. tostring(v))) .. "|")
end
local fmt = string.format
fprobe("TS51", function() return tostring(51.0) end)
fprobe("TS01", function() return tostring(0.1) end)
fprobe("TS1EM5", function() return tostring(1e-5) end)
fprobe("TS123456", function() return tostring(123456.7) end)
fprobe("TSTHIRD", function() return tostring(1 / 3) end)
fprobe("TS1E6", function() return tostring(1e6) end)
fprobe("TS1E7", function() return tostring(1e7) end)
fprobe("TS2P30", function() return tostring(2.0 ^ 30) end)
fprobe("TSNEG", function() return tostring(-2.5) end)
fprobe("TSNEGZ", function() return tostring(-0.0) end)
fprobe("TSINF", function() return tostring(math.huge) .. " " .. tostring(-math.huge) end)
fprobe("TSNAN", function() return tostring(0 / 0) end)
-- Subnormals built from their bit patterns. KNOWN DEVICE DIFFERENCE, not a
-- formatting one: Lua hands floats to its printers through C varargs, i.e.
-- promoted to double, and the RP2350's float->double conversion (the DCP)
-- flushes subnormals to zero; strtod->float does the same to a literal like
-- 1e-38. So on hardware TSMIN and DENORM read "0.0" (DENORM's "false" shows
-- the value itself is still nonzero).
fprobe("TSMIN", function()
    return tostring((string.unpack("<f", "\xee\xe3\x6c\x00"))) .. " "
        .. tostring((string.unpack("<f", "\x01\x00\x00\x00")))
end)
fprobe("DENORM", function()
    local tiny = string.unpack("<f", "\x01\x00\x00\x00")
    return tostring(1e-38) .. " " .. tostring(tiny * 1.0) .. " " .. tostring(tiny == 0)
end)
fprobe("TSMAX", function() return tostring(3.4028235e38) end)
fprobe("CONCAT", function() return "v=" .. 2.5 .. "," .. 7.0 end)
fprobe("SFG", function() return fmt("%g", 2.5) end)
fprobe("SF3F", function() return fmt("%.3f", 1 / 3) end)
fprobe("SF51F", function() return fmt("%5.1f", 2.25) end)
fprobe("SFF", function() return fmt("%f", 1.5) end)
fprobe("SFE", function() return fmt("%e", 12345.678) end)
fprobe("SF12F", function() return fmt("%.12f", 0.1) end)
fprobe("SFBIGF", function() return fmt("%.1f", 1e10) end)
fprobe("SFG3", function() return fmt("%.3g", 12345) end)
fprobe("SFGHASH", function() return fmt("%#g", 1.0) end)
fprobe("SFGRANGE", function()
    return fmt("%g %g %g %g %g", 0.0001, 1e-5, 100000, 1e6, 0)
end)
fprobe("SFE0", function() return fmt("%.0e %.0e", 25, 35) end)
fprobe("SFF0", function() return fmt("%.0f %.0f %.0f %.0f", 0.5, 1.5, 2.5, -0.5) end)
fprobe("SFFLAGS", function()
    return fmt("%-8.2f|%+08.2f|% .1e|%010.3g|%-+9.1E|", 3.14159, -2.5, 100, 0.000123456, 5)
end)
fprobe("SFG99", function() return fmt("%.20g", 0.1) end)
fprobe("SFINF", function() return fmt("%f %e %g %5.1f %-6g|", math.huge, -math.huge, 0 / 0, math.huge, -math.huge) end)
fprobe("SFA", function() return fmt("%a %A %.2a", 1.0, 0.1, 1 / 3) end)
fprobe("SFQ", function() return fmt("%q", 0.1) end)
fprobe("SFINT", function() return fmt("%5.3d|%#x|%+d|%-4i|%c|%o", 7, 255, 3, 2, 65, 8) end)
fprobe("JSON", function() return picocalc.json.encode({ 51.0, 100.0, 1e-7, 2.5, 1 / 3 }) end)

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
-- 5000, not 20000: 20000 exhausted the device's PSRAM heap (Lua OOM)
for i = 1, 5000 do t[i] = { i, i * 0.5, "s" .. i } end
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

-- 6 ── Lua -> C -> Lua recursion (string.gsub callbacks) stops at
--      LUAI_MAXCCALLS=60 with a clean "C stack overflow" error. On hardware
--      each level costs ~870 bytes of C stack; the 64 KB Lua VM stack holds
--      the full 60, so the count limit trips before the stack limit.
local gdepth = 0
local function grec(n)
    gdepth = math.max(gdepth, n)
    return (string.gsub("a", "a", function() return grec(n + 1) end))
end
local function gsub_to(n)
    if n == 0 then return "x" end
    return (string.gsub("a", "a", function() return gsub_to(n - 1) end))
end
probe("GSUB40", function() return gsub_to(40) end)
local gok, gerr = pcall(grec, 1)
log("LR GSUBDEEP ok=" .. tostring(gok) .. " cstack="
    .. tostring(type(gerr) == "string" and gerr:find("C stack overflow") ~= nil))
log("LR GSUBDEPTH " .. gdepth)
log("LR GSUBERR " .. tostring(gerr))

log("LR DONE")
local path = picocalc.fs.appPath("lr.txt")
local f = path and picocalc.fs.open(path, "w")
if f then
    picocalc.fs.write(f, table.concat(lines, "\n") .. "\n")
    picocalc.fs.close(f)
end
sys.log("LR SAVED " .. tostring(f ~= nil))
