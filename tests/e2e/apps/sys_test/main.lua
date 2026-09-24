-- System functions test fixture (picotest kit).
-- Exercises picocalc.sys.* and the per-app picocalc.config store.

local pc = picocalc
local sys = pc.sys
local T = sys.loadlib("picotest")

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Running sys tests...", pc.display.WHITE)
pc.display.flush()

T.case("getTimeMs", function()
    local t1 = sys.getTimeMs()
    T.ok(math.type(t1) == "integer" and t1 > 0, "getTimeMs=" .. tostring(t1))
    sys.sleep(50)
    local elapsed = sys.getTimeMs() - t1
    T.ok(elapsed >= 40 and elapsed <= 500, "elapsed=" .. elapsed)
end)

T.case("sleep", function()
    local t1 = sys.getTimeMs()
    sys.sleep(100)
    local elapsed = sys.getTimeMs() - t1
    T.ok(elapsed >= 80 and elapsed <= 500, "elapsed=" .. elapsed)
end)

T.case("getVersion", function()
    local v = sys.getVersion()
    T.eq(type(v), "string")
    T.ok(#v > 0, "empty version")
end)

T.case("log", function()
    -- The harness checks that this marker reaches the log buffer.
    sys.log("LOG_MARKER_" .. tostring(sys.getTimeMs()))
end)

T.case("getClock", function()
    local c = sys.getClock()
    T.eq(type(c), "table")
    T.eq(math.type(c.hour), "integer", "hour")
    T.eq(math.type(c.min), "integer", "min")
    T.eq(math.type(c.sec), "integer", "sec")
    T.ok(c.hour >= 0 and c.hour <= 23, "hour=" .. c.hour)
    T.ok(c.min >= 0 and c.min <= 59, "min=" .. c.min)
end)

T.case("getMemInfo", function()
    local m = sys.getMemInfo()
    T.eq(type(m), "table")
    for _, k in ipairs({ "psram_free", "psram_total", "sram_free" }) do
        T.eq(type(m[k]), "number", k)
    end
    T.ok(m.psram_total >= 0 and m.psram_free >= 0, "negative psram figures")
end)

T.case("config", function()
    local cfg = pc.config
    T.ok(cfg, "picocalc.config missing")
    cfg.set("e2e_test_key", "hello_e2e")
    T.eq(cfg.get("e2e_test_key"), "hello_e2e", "set/get")
    cfg.set("e2e_test_key", "updated")
    T.eq(cfg.get("e2e_test_key"), "updated", "overwrite")
    T.eq(cfg.get("nonexistent_key_xyz"), nil, "missing key")
end)

-- Persistence across launches: reload from disk, report the previous run's
-- counter (the harness checks it), bump it and save.
T.case("config_persist", function()
    local cfg = pc.config
    cfg.load()
    local prev = tonumber(cfg.get("e2e_runs") or "0")
    sys.log("SYS:RUNS prev=" .. prev)
    cfg.set("e2e_runs", tostring(prev + 1))
    T.ok(cfg.save(), "config.save() failed")
end)

T.case("exit_sentinel", function()
    -- sys.exit would end the app; only check it exists.
    T.eq(type(sys.exit), "function")
end)

T.done()
