-- records.lua — per-puzzle completion records.
--
-- Persists to /data/com.picos.nonogram/records.json:
--
--   { "v":1, "records": { "<puzzleId>": {"solved":true,"secs":42,"moves":9,"plays":3} } }
--
-- Nested tables are why this needs picocalc.json rather than game.save: the
-- latter's encoder wrote `null` for any non-scalar, so a per-puzzle map was
-- impossible to store until that was fixed.
--
-- Everything degrades to in-memory-only when picocalc.json is absent (older
-- firmware): the tick still appears within a session, it just does not survive
-- a relaunch. That keeps the caller free of capability checks.

local pc = picocalc
local fs = pc.fs

local R = {}

local FILE = "records.json"
local data = nil          -- { v = 1, records = { [id] = rec } }
local dirty = false
local persistent = false

local function jsonlib()
    return (type(pc.json) == "table") and pc.json or nil
end

local function path()
    -- appPath creates /data/<APP_ID> as a side effect and returns a full path.
    return fs.appPath(FILE)
end

function R.available()
    return persistent
end

function R.load()
    data = { v = 1, records = {} }
    dirty = false

    local J = jsonlib()
    persistent = (J ~= nil)
    if not J then
        pc.sys.log("NG:RECORDS in-memory only (no picocalc.json)")
        return data
    end

    local p = path()
    local src = fs.readFile(p)
    if not src or #src == 0 then return data end

    local decoded, err = J.decode(src)
    if type(decoded) ~= "table" then
        -- A corrupt file must not wipe progress silently or take the app down.
        -- Keep the bad file so it can be inspected, and start fresh in memory.
        pc.sys.log("NG:RECORDS corrupt (" .. tostring(err) .. ")")
        return data
    end

    if type(decoded.records) == "table" then
        data.records = decoded.records
    end
    return data
end

local function ensure()
    if not data then R.load() end
    return data
end

-- get(id) -> record or nil.  Record fields: solved, secs, moves, plays.
function R.get(id)
    local d = ensure()
    return d.records[id]
end

function R.isSolved(id)
    local rec = R.get(id)
    return rec ~= nil and rec.solved == true
end

function R.bestSecs(id)
    local rec = R.get(id)
    return rec and rec.secs or nil
end

-- Counts an attempt. Kept separate from markSolved so a puzzle opened and
-- abandoned still registers.
function R.markPlayed(id)
    local d = ensure()
    local rec = d.records[id]
    if not rec then
        rec = { solved = false, plays = 0 }
        d.records[id] = rec
    end
    rec.plays = (rec.plays or 0) + 1
    dirty = true
end

-- Records a completion, keeping the best time and move count.
-- Returns isNewBest, isFirstSolve.
function R.markSolved(id, secs, moves)
    local d = ensure()
    local rec = d.records[id]
    if not rec then
        rec = { plays = 1 }
        d.records[id] = rec
    end

    local firstSolve = (rec.solved ~= true)
    local newBest = false

    rec.solved = true
    if type(rec.secs) ~= "number" or secs < rec.secs then
        rec.secs = secs
        newBest = not firstSolve
    end
    if type(rec.moves) ~= "number" or moves < rec.moves then
        rec.moves = moves
    end

    dirty = true
    R.save()
    return newBest, firstSolve
end

function R.save()
    if not dirty then return true end
    local J = jsonlib()
    if not J then
        dirty = false     -- nothing to write to; stay in memory
        return false
    end

    local d = ensure()
    local ok, enc = pcall(J.encode, { v = 1, records = d.records })
    if not ok then
        pc.sys.log("NG:RECORDS encode failed: " .. tostring(enc))
        return false
    end

    local p = path()
    local fh = fs.open(p, "w")
    if not fh then
        pc.sys.log("NG:RECORDS cannot write " .. p)
        return false
    end
    fs.write(fh, enc)
    fs.close(fh)
    dirty = false
    return true
end

-- Aggregate counts for the menu header.
function R.summary()
    local d = ensure()
    local solved, played = 0, 0
    for _, rec in pairs(d.records) do
        if rec.solved then solved = solved + 1 end
        played = played + (rec.plays or 0)
    end
    return solved, played
end

function R.reset(id)
    local d = ensure()
    if id then d.records[id] = nil else d.records = {} end
    dirty = true
    R.save()
end

-- mm:ss for display; falls back to raw seconds past an hour.
function R.formatTime(secs)
    if type(secs) ~= "number" then return "--:--" end
    if secs >= 3600 then return ("%dh"):format(secs // 3600) end
    return ("%d:%02d"):format(secs // 60, secs % 60)
end

return R
