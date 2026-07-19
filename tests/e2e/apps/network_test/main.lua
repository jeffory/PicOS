-- Network test fixture
-- Exercises picocalc.network.* APIs
-- Each test logs "PASS:<name>" or "FAIL:<name>:<reason>"

local pc = picocalc
local net = pc.network
local log = pc.sys.log

local function test_getStatus()
    local status = net.getStatus()
    if type(status) == "number" then
        -- Should be one of the status constants
        if status == net.kStatusNotConnected or
           status == net.kStatusConnected or
           status == net.kStatusNotAvailable then
            return "PASS:getStatus:" .. tostring(status)
        else
            return "FAIL:getStatus:unknown_status=" .. tostring(status)
        end
    else
        return "FAIL:getStatus:invalid_type=" .. type(status)
    end
end

local function test_http_new_close()
    -- Test creating and closing an HTTP connection object
    -- Use a non-routable IP to avoid actual network traffic
    local ok, conn = pcall(function()
        return net.http.new("192.0.2.1", 80, false, "e2e_test")
    end)
    if not ok then
        return "FAIL:http_new:" .. tostring(conn)
    end
    if conn == nil then
        return "FAIL:http_new:nil_connection"
    end

    -- Close the connection
    local close_ok, close_err = pcall(function()
        conn:close()
    end)
    if close_ok then
        return "PASS:http_new_close"
    else
        return "FAIL:http_new_close:" .. tostring(close_err)
    end
end

local function test_http_error_handling()
    -- Verify getError works on a connection
    local ok, conn = pcall(function()
        return net.http.new("192.0.2.1", 80, false, "e2e_error_test")
    end)
    if not ok then
        return "FAIL:http_error_handling:" .. tostring(conn)
    end

    -- getError should return nil or a string (no crash)
    local err_ok, err = pcall(function()
        return conn:getError()
    end)
    if err_ok then
        pcall(function() conn:close() end)
        return "PASS:http_error_handling"
    else
        pcall(function() conn:close() end)
        return "FAIL:http_error_handling:" .. tostring(err)
    end
end

-- Run all tests
local tests = {
    test_getStatus,
    test_http_new_close,
    test_http_error_handling,
}

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Running network tests...", pc.display.WHITE)
pc.display.flush()

for _, test in ipairs(tests) do
    local ok, result = pcall(test)
    if ok then
        log(result)
    else
        log("FAIL:exception:" .. tostring(result))
    end
end

log("NETWORK_TESTS_DONE")
pc.sys.sleep(100)
