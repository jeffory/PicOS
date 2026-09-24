-- E2E fixture for the Lua heap's small-object pools (src/os/small_alloc.c).
--
-- umm hands out 200-byte blocks and its 15-bit block index caps the device
-- heap at ~30,800 of them, so before the pools every Lua object - a 32-byte
-- table, a 20-byte string - cost a whole block and 20,000 small tables
-- ({i, i*0.5, "s"..i} is three objects) ran the device out of memory. Each
-- phase logs "SA <NAME> <value>"; the test asserts on them. Under the
-- simulator's --real-umm the heap is the device's own umm, so LIVE20K fails
-- without the pools exactly as it did on hardware.
local sys = picocalc.sys
local lines = {}
local function log(s)
    lines[#lines + 1] = s
    sys.log(s)
end

local function mem(tag)
    collectgarbage("collect")
    local m = sys.getMemInfo()
    log(string.format("SA MEM_%s free=%d largest=%d frag=%d gc_kb=%.1f "
        .. "slabs=%s slab_bytes=%s objects=%s object_bytes=%s", tag,
        m.psram_free, m.psram_largest_block, m.psram_fragmentation,
        collectgarbage("count"), tostring(m.small_pool_slabs),
        tostring(m.small_pool_bytes), tostring(m.small_pool_objects),
        tostring(m.small_pool_object_bytes)))
    return m
end

local function phase(name, fn)
    local ok, err = pcall(fn)
    log("SA " .. name .. " " .. (ok and "ok" or ("ERR " .. tostring(err))))
end

local start = mem("START")

-- 1: the Task 30 case - 20,000 small tables held at once (60,000 objects)
phase("LIVE20K", function()
    local t = {}
    for i = 1, 20000 do t[i] = { i, i * 0.5, "s" .. i } end
    mem("LIVE20K")
    local sum = 0
    for i = 1, 20000 do sum = sum + t[i][1] end
    assert(sum == 20000 * 20001 // 2, "table contents")
end)
collectgarbage("collect")

-- 2: churn - 100,000 small tables and 100,000 strings, made and dropped in
--    batches, so the pools fill, empty and refill
phase("CHURN100K", function()
    local made = 0
    for batch = 1, 20 do
        local keep = {}
        for i = 1, 5000 do
            local n = (batch - 1) * 5000 + i
            keep[i] = { n, tostring(n) .. ":" .. batch }
            made = made + 1
        end
        assert(keep[5000][1] == batch * 5000, "batch contents")
        keep = nil
        if batch % 5 == 0 then collectgarbage("collect") end
    end
    assert(made == 100000)
end)

-- 3: empty tables - freeing one frees its absent array part as
--    realloc(NULL, 0), which must not allocate (it leaked a block each)
phase("EMPTY20K", function()
    local t = {}
    for i = 1, 20000 do t[i] = {} end
    t = nil
    collectgarbage("collect")
end)

-- 4: tables growing through every size class and past it (realloc across
--    classes and out of the pools), strings of every short length
phase("GROW", function()
    local t = {}
    for i = 1, 2000 do
        local g = {}
        for k = 1, (i % 40) + 1 do g[k] = k end
        t[i] = g
    end
    for i = 1, 2000 do
        local g = t[i]
        assert(#g == (i % 40) + 1, "grown table length")
    end
    local s = {}
    for len = 0, 80 do s[#s + 1] = string.rep("x", len) .. len end
    for len = 0, 80 do assert(s[len + 1] == string.rep("x", len) .. len) end
end)

local finish = mem("END")
-- Slabs go back to umm as they empty: after a full collect the heap is
-- within a few slabs of where it started.
log("SA RETURNED_KB " .. ((start.psram_free - finish.psram_free) // 1024))

-- Task 1 numeric loop, for before/after timing (logged, not asserted)
local t0 = sys.getTimeMs()
local acc = 0.0
for i = 1, 2000000 do acc = acc + (i % 7) * 0.5 end
log("SA BENCH_LOOP_MS " .. (sys.getTimeMs() - t0))

log("SA DONE")
local path = picocalc.fs.appPath("sa.txt")
local f = path and picocalc.fs.open(path, "w")
if f then
    picocalc.fs.write(f, table.concat(lines, "\n") .. "\n")
    picocalc.fs.close(f)
end
