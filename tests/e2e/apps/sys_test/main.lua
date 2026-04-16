-- System functions test fixture
-- Exercises picocalc.sys.* and picocalc.config.* APIs
-- Each test logs "PASS:<name>" or "FAIL:<name>:<reason>"

local pc = picocalc
local sys = pc.sys
local log = sys.log

local function test_getTimeMs()
    local t1 = sys.getTimeMs()
    if type(t1) ~= "number" or t1 <= 0 then
        return "FAIL:getTimeMs:invalid_type_or_value"
    end
    sys.sleep(50)
    local t2 = sys.getTimeMs()
    local elapsed = t2 - t1
    if elapsed >= 40 and elapsed <= 500 then
        return "PASS:getTimeMs"
    else
        return "FAIL:getTimeMs:elapsed=" .. tostring(elapsed)
    end
end

local function test_sleep()
    local t1 = sys.getTimeMs()
    sys.sleep(100)
    local t2 = sys.getTimeMs()
    local elapsed = t2 - t1
    if elapsed >= 80 and elapsed <= 500 then
        return "PASS:sleep"
    else
        return "FAIL:sleep:elapsed=" .. tostring(elapsed)
    end
end

local function test_getVersion()
    local v = sys.getVersion()
    if type(v) == "string" and #v > 0 then
        return "PASS:getVersion"
    else
        return "FAIL:getVersion:invalid=" .. tostring(v)
    end
end

local function test_log_roundtrip()
    -- Log a unique marker and rely on E2E test to verify it appears
    local marker = "LOG_MARKER_" .. tostring(sys.getTimeMs())
    log(marker)
    -- Can't verify from Lua side, but the E2E test checks log buffer
    return "PASS:log:" .. marker
end

local function test_getClock()
    local c = sys.getClock()
    if type(c) ~= "table" then
        return "FAIL:getClock:not_table"
    end
    if type(c.hour) ~= "number" then return "FAIL:getClock:no_hour" end
    if type(c.min) ~= "number" then return "FAIL:getClock:no_min" end
    if type(c.sec) ~= "number" then return "FAIL:getClock:no_sec" end
    if c.hour >= 0 and c.hour <= 23 and c.min >= 0 and c.min <= 59 then
        return "PASS:getClock"
    else
        return "FAIL:getClock:invalid_values"
    end
end

local function test_getMemInfo()
    local m = sys.getMemInfo()
    if type(m) ~= "table" then
        return "FAIL:getMemInfo:not_table"
    end
    if type(m.psram_free) ~= "number" then return "FAIL:getMemInfo:no_psram_free" end
    if type(m.psram_total) ~= "number" then return "FAIL:getMemInfo:no_psram_total" end
    if type(m.sram_free) ~= "number" then return "FAIL:getMemInfo:no_sram_free" end
    if m.psram_total >= 0 and m.psram_free >= 0 then
        return "PASS:getMemInfo"
    else
        return "FAIL:getMemInfo:invalid_values"
    end
end

local function test_config()
    local cfg = pc.config
    if not cfg then return "FAIL:config:nil_module" end

    -- Set a test key
    cfg.set("e2e_test_key", "hello_e2e")
    local val = cfg.get("e2e_test_key")
    if val ~= "hello_e2e" then
        return "FAIL:config:set_get_mismatch=" .. tostring(val)
    end

    -- Overwrite
    cfg.set("e2e_test_key", "updated")
    val = cfg.get("e2e_test_key")
    if val ~= "updated" then
        return "FAIL:config:overwrite_mismatch=" .. tostring(val)
    end

    -- Non-existent key returns nil
    val = cfg.get("nonexistent_key_xyz")
    if val ~= nil then
        return "FAIL:config:nonexistent_not_nil=" .. tostring(val)
    end

    return "PASS:config"
end

local function test_exit_sentinel()
    -- This test verifies the exit sentinel works by NOT calling sys.exit()
    -- (that would terminate the app). Instead we verify the function exists.
    if type(sys.exit) ~= "function" then
        return "FAIL:exit_sentinel:not_function"
    end
    return "PASS:exit_sentinel"
end

-- Run all tests
local tests = {
    test_getTimeMs,
    test_sleep,
    test_getVersion,
    test_log_roundtrip,
    test_getClock,
    test_getMemInfo,
    test_config,
    test_exit_sentinel,
}

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Running sys tests...", pc.display.WHITE)
pc.display.flush()

for _, test in ipairs(tests) do
    local ok, result = pcall(test)
    if ok then
        log(result)
    else
        log("FAIL:exception:" .. tostring(result))
    end
end

log("SYS_TESTS_DONE")
sys.sleep(100)
