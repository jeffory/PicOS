-- PicOS download library — stream an HTTP(S) response straight to a file.
-- Load with: local download = picocalc.sys.loadlib("download")
--
--   local ok, err = download.toFile(url, dest_path, {
--       onProgress   = function(received, total) end, -- total 0 if unknown
--       headers      = { ["X-Extra"] = "..." },       -- merged into request
--       timeoutMs    = 120000,                        -- whole-transfer cap
--       maxRedirects = 5,
--   })
--
-- Blocking: returns only when the transfer finished, failed, or timed out.
-- Chunks are written to the SD card as they arrive under fs.setSlowMode(true)
-- (SPI0 drops to 1 MHz — CYW43 radio EMI corrupts 25 MHz writes), so peak
-- memory stays one read-buffer regardless of file size. On any failure the
-- partial file is deleted.
--
-- The caller's app needs the "http" requirement and a connected network.
-- HTTP callbacks are fired from the Lua debug hook, so this library must be
-- called from plain Lua code (not from inside another network callback).

local pc   = picocalc
local fs   = pc.fs
local sys  = pc.sys
local net  = pc.network

local D = {}

local function parse_url(url)
    local host, path
    local ssl = false
    local port = 80

    if url:match("^https://") then
        ssl = true; port = 443
        local rest = url:sub(9)
        host = rest:match("^([^/]+)")
        path = rest:match("^[^/]+(/.*)") or "/"
    elseif url:match("^http://") then
        local rest = url:sub(8)
        host = rest:match("^([^/]+)")
        path = rest:match("^[^/]+(/.*)") or "/"
    else
        return nil
    end

    local h, p = host:match("^(.+):(%d+)$")
    if h then host = h; port = tonumber(p) end
    return host, port, ssl, path
end

-- Ensure every directory component of dest exists.
local function ensure_parent_dirs(dest)
    local dir = dest:match("^(.+)/[^/]+$")
    if not dir or dir == "" then return end
    local built = ""
    for part in dir:gmatch("[^/]+") do
        built = built .. "/" .. part
        fs.mkdir(built)
    end
end

-- One request/response leg. Returns:
--   "done"               transfer complete, file closed
--   "redirect", location follow the Location header
--   nil, err             failure (partial file already cleaned up)
local function fetch_leg(url, dest, opts, deadline)
    local host, port, ssl, path = parse_url(url)
    if not host then return nil, "invalid URL: " .. tostring(url) end

    local conn = net.http.new(host, port, ssl, "download.lua")
    if not conn then return nil, "cannot connect to " .. host end

    conn:setConnectTimeout(30)
    conn:setReadTimeout(60)
    conn:setReadBufferSize(32 * 1024)

    local state = {
        done = false, err = nil, redirect = nil,
        wf = nil, received = 0, total = 0, status = nil,
    }

    local function cleanup_partial()
        if state.wf then fs.close(state.wf); state.wf = nil end
        fs.setSlowMode(false)
        fs.delete(dest)
    end

    local function fail(err)
        if state.done then return end
        state.err = err
        state.done = true
        cleanup_partial()
    end

    conn:setHeadersReadCallback(function()
        local hdrs = conn:getResponseHeaders()
        if hdrs and hdrs["content-length"] then
            local cl = tonumber(hdrs["content-length"])
            if cl and cl > 0 then state.total = cl end
        end
    end)

    conn:setRequestCallback(function()
        if state.done then return end
        local status = conn:getResponseStatus()
        state.status = status
        if status and status ~= 200 then return end -- redirect/error: drain later

        if not state.wf then
            ensure_parent_dirs(dest)
            if not fs.ensureReady() then
                fail("SD card not responding")
                conn:close()
                return
            end
            fs.setSlowMode(true) -- radio is active for the whole transfer
            state.wf = fs.open(dest, "w")
            if not state.wf then
                fail("cannot open " .. dest)
                conn:close()
                return
            end
        end

        while true do
            local avail = conn:getBytesAvailable()
            if avail <= 0 then break end
            local data = conn:read()
            if not data or #data == 0 then break end
            fs.write(state.wf, data)
            state.received = state.received + #data
        end
        if opts.onProgress then
            pcall(opts.onProgress, state.received, state.total)
        end
    end)

    conn:setRequestCompleteCallback(function()
        if state.done then return end
        local status = conn:getResponseStatus()
        local conn_err = conn:getError()

        if status and status >= 300 and status < 400 then
            local hdrs = conn:getResponseHeaders()
            local location = hdrs and hdrs["location"]
            if state.wf then fs.close(state.wf); state.wf = nil end
            fs.setSlowMode(false)
            fs.delete(dest)
            if location then
                state.redirect = location
            else
                state.err = "redirect without Location"
            end
            state.done = true
            return
        end

        if conn_err then return fail(conn_err) end
        if status and status ~= 200 then
            return fail("HTTP " .. tostring(status))
        end
        if not state.wf then
            return fail("empty response")
        end

        fs.close(state.wf); state.wf = nil
        fs.setSlowMode(false)
        if opts.onProgress then
            pcall(opts.onProgress, state.received,
                  state.total > 0 and state.total or state.received)
        end
        state.done = true
    end)

    conn:setConnectionClosedCallback(function()
        if not state.done then fail("connection closed") end
    end)

    local headers = { ["User-Agent"] = "PicOS-download/1.0" }
    if opts.headers then
        for k, v in pairs(opts.headers) do headers[k] = v end
    end
    conn:get(path, headers)

    -- Block until a callback resolves the transfer. Callbacks fire from the
    -- Lua debug hook, which needs Lua opcodes executing — hence the loop.
    while not state.done do
        if sys.getTimeMs() > deadline then
            conn:close()
            fail("timeout")
            break
        end
        sys.sleep(5)
    end
    conn:close()

    if state.redirect then return "redirect", state.redirect end
    if state.err then return nil, state.err end
    return "done"
end

function D.toFile(url, dest, opts)
    opts = opts or {}
    local max_redirects = opts.maxRedirects or 5
    local deadline = sys.getTimeMs() + (opts.timeoutMs or 120000)

    for _ = 0, max_redirects do
        local res, arg = fetch_leg(url, dest, opts, deadline)
        if res == "done" then return true end
        if res == "redirect" then
            url = arg
        else
            return false, arg
        end
    end
    return false, "too many redirects"
end

return D
