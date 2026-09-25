-- picotest — the E2E suite's Lua test kit (tests/e2e/lib/picotest.lua).
--
-- The harness stages this file as /system/lib/picotest.lua. Load it with
--
--     local T = picocalc.sys.loadlib("picotest")
--     T.case("adds", function() T.eq(1 + 1, 2) end)
--     T.case("needs wifi", function() T.skip("no network in the sim") end)
--     T.done()
--
-- Every case reports on two channels:
--   * the log:  "[T] CASE <name> PASS|FAIL|SKIP <detail>", then
--               "[T] DONE pass=N fail=M skip=K" from T.done()
--   * a file:   /data/<APP_ID>/test_results.json, rewritten after every
--               case (so a crash mid-suite still leaves the finished cases)
--               and marked "done":true by T.done(). The file is the robust
--               channel on hardware, where serial capture drops lines.
--
-- Cases run in order under pcall. A failed check raises, so a case stops at
-- its first failure. The app's identity globals (APP_ID, APP_DIR, APP_NAME,
-- APP_REQUIREMENTS) are restored after every case, so a case that tampers
-- with them (the sandbox tests do) cannot break later cases or the results
-- file. The exit sentinel (sys.exit, menu Exit, exit_app) is re-raised, so
-- the kit never keeps an app alive that was asked to exit.

local T = {}

local log = picocalc.sys.log
local json = picocalc.json
local fs = picocalc.fs

local SKIP = {}        -- metatable tag for T.skip's error object

local cases = {}
local counts = { pass = 0, fail = 0, skip = 0 }
local done = false

-- Snapshot of the identity globals taken when the kit is loaded.
local function copy(t)
    if type(t) ~= "table" then return t end
    local c = {}
    for k, v in pairs(t) do c[k] = v end
    return c
end

local ident = {
    APP_ID = APP_ID, APP_DIR = APP_DIR, APP_NAME = APP_NAME,
    req = APP_REQUIREMENTS, req_fields = copy(APP_REQUIREMENTS),
}

local function restore_identity()
    APP_ID, APP_DIR, APP_NAME = ident.APP_ID, ident.APP_DIR, ident.APP_NAME
    if type(ident.req) == "table" then
        for k in pairs(ident.req) do ident.req[k] = nil end
        for k, v in pairs(ident.req_fields) do ident.req[k] = v end
    end
    APP_REQUIREMENTS = ident.req
end

T.app_id = ident.APP_ID

local function show(v)
    if type(v) == "string" then
        if #v > 80 then
            return string.format("%q", v:sub(1, 80)) .. "... (" .. #v .. " bytes)"
        end
        return string.format("%q", v)
    end
    return tostring(v)
end

-- ── Checks ──────────────────────────────────────────────────────────────────
-- A failed check raises a string with error(msg, 2), so the detail starts
-- with the position of the check in the test ("main.lua:12: ...").

function T.eq(got, want, msg)
    if got ~= want then
        error((msg and (msg .. ": ") or "") ..
              "expected " .. show(want) .. ", got " .. show(got), 2)
    end
    return got
end

function T.ok(v, msg)
    if not v then
        error(msg or ("expected a true value, got " .. show(v)), 2)
    end
    return v
end

function T.fail(msg)
    error(msg or "failed", 2)
end

-- fn must raise; if `pattern` is given the error text must match it (a Lua
-- pattern). Returns the error value.
function T.raises(fn, pattern)
    local ok, err = pcall(fn)
    if ok then error("expected an error, none raised", 2) end
    if type(err) == "userdata" then error(err, 0) end  -- exit sentinel
    if pattern and not tostring(err):find(pattern) then
        error("error " .. show(tostring(err)) ..
              " does not match " .. show(pattern), 2)
    end
    return err
end

-- Ends the current case as SKIP. The harness fails a run that skips unless
-- the case is on tests/e2e/skip_allowlist.txt.
function T.skip(reason)
    error(setmetatable({ msg = reason or "skipped" }, SKIP), 0)
end

-- ── Results file ────────────────────────────────────────────────────────────

local function write_results()
    local doc = {
        app = ident.APP_ID, done = done,
        pass = counts.pass, fail = counts.fail, skip = counts.skip,
        cases = cases,
    }
    local ok, text = pcall(json.encode, doc)
    if not ok then
        log("[T] ERROR cannot encode results: " .. tostring(text))
        return
    end
    local path = fs.appPath("test_results.json")
    local f = path and fs.open(path, "w")
    if not f then
        log("[T] ERROR cannot write " .. tostring(path))
        return
    end
    fs.write(f, text)
    fs.close(f)
end

-- ── Cases ───────────────────────────────────────────────────────────────────

local function one_line(s)
    return (tostring(s):gsub("[\r\n]+", " | "))
end

function T.case(name, fn)
    local ok, err = pcall(fn)
    restore_identity()
    if not ok and type(err) == "userdata" then
        error(err, 0)  -- exit sentinel: let the app exit
    end
    local status, detail
    if ok then
        status, detail = "PASS", ""
    elseif getmetatable(err) == SKIP then
        status, detail = "SKIP", err.msg
    else
        status, detail = "FAIL", tostring(err)
    end
    detail = one_line(detail)
    counts[status:lower()] = counts[status:lower()] + 1
    cases[#cases + 1] = { name = name, status = status, detail = detail }
    log("[T] CASE " .. name .. " " .. status .. (detail ~= "" and (" " .. detail) or ""))
    write_results()
    return status == "PASS"
end

function T.done()
    done = true
    write_results()
    log(string.format("[T] DONE pass=%d fail=%d skip=%d",
                      counts.pass, counts.fail, counts.skip))
    return counts
end

return T
