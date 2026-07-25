-- net_client.lua — HTTP client for the share API in API.md.
--
-- Non-blocking: one request in flight, driven by net.update() from the frame
-- loop. picocalc's HTTP callbacks fire from the Lua opcode hook, so simply
-- continuing to run Lua pumps them; there is nothing to poll.
--
-- Three constraints from the underlying API that are easy to get wrong:
--
--   * The host is a BARE hostname, no scheme. Port defaults to 443 when ssl is
--     set, else 80.
--   * conn:post is ARITY-SWITCHED. post(path, body) treats argument 2 as the
--     body; post(path, headers, body) treats 2 as headers. Dropping the headers
--     table silently sends the headers string as the body.
--   * All callbacks MUST be registered before issuing the request, and they take
--     zero arguments — data is pulled with the getters.

local pc  = picocalc
local sys = pc.sys

local N = {}

local cfg = {
    host = nil,
    port = nil,
    ssl  = true,
    base = "/v1",
    token = nil,
}

local req = nil     -- in-flight request state

local TIMEOUT_MS = 15000

function N.configure(t)
    cfg.host  = t.host
    cfg.port  = t.port
    cfg.ssl   = (t.ssl ~= false)
    cfg.base  = t.base or "/v1"
    cfg.token = t.token
end

function N.configured()
    return cfg.host ~= nil and #cfg.host > 0
end

function N.busy()
    return req ~= nil
end

function N.status()
    if not req then return "idle" end
    return req.phase
end

-- Reasons a request cannot even be attempted, reported up front so the UI can
-- explain itself instead of just failing.
local function preflight()
    if not N.configured() then return "no server configured" end
    if not APP_REQUIREMENTS or not APP_REQUIREMENTS.http then
        return "app lacks the http requirement"
    end
    if pc.network.getStatus() ~= pc.network.kStatusConnected then
        return "no network"
    end
    return nil
end

local function headers(extra)
    local h = { ["X-Client"] = "neurogram/1.0", ["Accept"] = "application/json" }
    if cfg.token then h["Authorization"] = "Bearer " .. cfg.token end
    if extra then
        for k, v in pairs(extra) do h[k] = v end
    end
    return h
end

local function finish(ok, result, err)
    local cb = req and req.cb
    local conn = req and req.conn
    req = nil
    if conn then pcall(conn.close, conn) end
    if cb then cb(ok, result, err) end
end

-- Decodes the accumulated body, mapping the API's error envelope onto err.
local function complete()
    local body = table.concat(req.chunks)
    local status = req.status or 0

    if not pc.json then
        finish(false, nil, "picocalc.json unavailable")
        return
    end

    local data, derr = nil, nil
    if #body > 0 then
        data, derr = pc.json.decode(body)
    end

    if status >= 200 and status < 300 then
        if #body == 0 then finish(true, {}, nil); return end
        if not data then finish(false, nil, "bad JSON: " .. tostring(derr)); return end
        finish(true, data, nil)
        return
    end

    local msg = ("HTTP %d"):format(status)
    local code = nil
    if data and type(data.error) == "table" then
        code = data.error.code
        msg = tostring(data.error.message or code or msg)
    end
    finish(false, { status = status, code = code }, msg)
end

-- Starts a request. method is "GET" or "POST".
local function start(method, path, body, extraHeaders, cb)
    local why = preflight()
    if why then
        if cb then cb(false, nil, why) end
        return false, why
    end
    if req then
        if cb then cb(false, nil, "busy") end
        return false, "busy"
    end

    local port = cfg.port or (cfg.ssl and 443 or 80)
    local conn, cerr = pc.network.http.new(cfg.host, port, cfg.ssl, "neurogram")
    if not conn then
        if cb then cb(false, nil, cerr or "cannot open connection") end
        return false, cerr
    end

    req = {
        conn = conn, cb = cb, chunks = {}, status = nil,
        phase = "connecting", deadline = sys.getTimeMs() + TIMEOUT_MS,
    }

    conn:setConnectTimeout(10)
    conn:setReadTimeout(10)

    -- Registered BEFORE the request is issued, and all zero-argument.
    conn:setHeadersReadCallback(function()
        if not req then return end
        req.status = conn:getResponseStatus()
        req.phase = "reading"
    end)

    conn:setRequestCallback(function()
        if not req then return end
        local chunk = conn:read()
        while chunk do
            req.chunks[#req.chunks + 1] = chunk
            chunk = conn:read()
        end
    end)

    conn:setRequestCompleteCallback(function()
        if not req then return end
        req.phase = "complete"
        complete()
    end)

    conn:setConnectionClosedCallback(function()
        if not req then return end
        local e = conn:getError()
        if req.phase == "complete" then return end
        if e then
            finish(false, nil, e)
        else
            -- A close without an error after headers means the body ended.
            complete()
        end
    end)

    local full = cfg.base .. path
    local ok, err

    if method == "GET" then
        ok, err = conn:get(full, headers(extraHeaders))
    else
        local h = headers(extraHeaders)
        h["Content-Type"] = "application/json"
        h["Content-Length"] = tostring(#body)
        -- 4-argument form. Passing (path, body) here would send the headers as
        -- the body; see the note at the top of this file.
        ok, err = conn:post(full, h, body)
    end

    if not ok then
        finish(false, nil, err or "request failed")
        return false, err
    end

    req.phase = "sent"
    return true
end

-- Must be called each frame while a request is in flight. The HTTP callbacks
-- themselves are driven by the opcode hook; this only enforces the timeout.
function N.update()
    if not req then return end
    if sys.getTimeMs() > req.deadline then
        sys.log("NG:NET timeout")
        finish(false, nil, "timed out")
    end
end

function N.cancel()
    if req then finish(false, nil, "cancelled") end
end

-- ── Endpoints ────────────────────────────────────────────────────────────────

local function query(params)
    local parts = {}
    for k, v in pairs(params or {}) do
        if v ~= nil then
            parts[#parts + 1] = tostring(k) .. "=" .. tostring(v)
        end
    end
    if #parts == 0 then return "" end
    return "?" .. table.concat(parts, "&")
end

-- list({sort, size, limit, cursor}, cb) -> cb(ok, {items=, next=}, err)
function N.list(opts, cb)
    opts = opts or {}
    return start("GET", "/puzzles" .. query({
        sort = opts.sort, size = opts.size,
        limit = opts.limit or 20, cursor = opts.cursor,
    }), nil, nil, cb)
end

function N.fetch(id, cb)
    return start("GET", "/puzzles/" .. tostring(id), nil, nil, cb)
end

-- Publishes a puzzle. The grid travels as its share code; the server never
-- decodes it (see API.md).
function N.publish(puzzle, code, logic, cb)
    if not pc.json then
        if cb then cb(false, nil, "picocalc.json unavailable") end
        return false
    end

    local payload = pc.json.encode({
        name   = puzzle.name,
        author = puzzle.author or "",
        w      = puzzle.w,
        h      = puzzle.h,
        code   = code,
        logic  = logic or "unknown",
    })

    -- Idempotency key derived from the payload, so a retry after a timeout
    -- cannot create a second copy. Deterministic on purpose.
    local h = 0x811C9DC5
    for i = 1, #payload do
        h = h ~ payload:byte(i)
        h = (h * 16777619) & 0xFFFFFFFF
    end

    return start("POST", "/puzzles", payload,
                 { ["Idempotency-Key"] = ("ng-%08x"):format(h) }, cb)
end

-- Best-effort telemetry: failures are ignored by design.
function N.reportPlay(id, solved, seconds, moves, cb)
    if not pc.json then return false end
    local payload = pc.json.encode({
        solved = solved and true or false,
        seconds = seconds or 0,
        moves = moves or 0,
    })
    return start("POST", "/puzzles/" .. tostring(id) .. "/plays", payload, nil, cb)
end

function N.byCode(code, cb)
    return start("GET", "/puzzles/by-code/" .. tostring(code), nil, nil, cb)
end

return N
