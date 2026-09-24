-- Network test fixture (picotest kit). Offline: connections go to
-- 192.0.2.1 (TEST-NET-1) and are closed before any traffic.

local pc = picocalc
local net = pc.network
local T = pc.sys.loadlib("picotest")

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Running network tests...", pc.display.WHITE)
pc.display.flush()

T.case("getStatus", function()
    local status = net.getStatus()
    T.eq(math.type(status), "integer", "status type")
    T.ok(status == net.kStatusNotConnected or status == net.kStatusConnected
         or status == net.kStatusNotAvailable, "unknown status " .. status)
end)

T.case("http_new_close", function()
    local conn = T.ok(net.http.new("192.0.2.1", 80, false, "e2e_test"),
                      "http.new returned nil")
    conn:close()
end)

T.case("http_error_handling", function()
    local conn = T.ok(net.http.new("192.0.2.1", 80, false, "e2e_error_test"),
                      "http.new returned nil")
    local err = conn:getError()
    T.ok(err == nil or type(err) == "string", "getError returned " .. type(err))
    conn:close()
end)

-- TLS verification is on by default (firmware); setInsecure is the explicit,
-- per-connection opt-out for self-signed dev servers (API v8).
T.case("tls_set_insecure_optout", function()
    local conn = T.ok(net.http.new("192.0.2.1", 443, true, "e2e_tls"),
                      "http.new returned nil")
    T.eq(type(conn.setInsecure), "function", "http conn:setInsecure")
    conn:setInsecure(true)
    conn:setInsecure(false)
    conn:close()
    T.ok(pc.tcp, "picocalc.tcp")
    local sock = T.ok(pc.tcp.new("192.0.2.1", 443, true), "tcp.new returned nil")
    T.eq(type(sock.setInsecure), "function", "tcp:setInsecure")
    sock:setInsecure(true)
    sock:close()
end)

T.done()
