-- store.lua — the local puzzle library on disk.
--
-- Layout under the app's writable sandbox:
--   /data/com.picos.nonogram/puzzles/<id>.json
--
-- Puzzles are stored with the grid as a share code rather than as a 225-element
-- JSON array: it is far smaller, and it means the file format, the typed share
-- code and the API wire format are all the SAME string, so there is one codec
-- and one place for a bug to hide.
--
-- Note fs.appPath() only auto-creates /data/<APP_ID> itself, not subdirectories,
-- so puzzles/ has to be mkdir'd explicitly.

local pc = picocalc
local fs = pc.fs

local Clues = require("clues")
local Share = require("sharecode")

local ST = {}

local DIR = nil

local function json()
    return pc.json
end

function ST.available()
    return type(pc.json) == "table"
end

function ST.init()
    -- appPath creates /data/<APP_ID> as a side effect.
    local base = fs.appPath("x"):gsub("/x$", "")
    DIR = base .. "/puzzles"
    if not fs.exists(DIR) then fs.mkdir(DIR) end
    return DIR
end

function ST.dir()
    if not DIR then ST.init() end
    return DIR
end

-- Rebuilds the derived fields a puzzle needs in memory from its stored form.
local function hydrate(rec)
    local p, err = Share.decode(rec.code)
    if not p then return nil, err end

    p.id       = rec.id
    p.name     = rec.name or p.name or "UNTITLED"
    p.author   = rec.author
    p.created  = rec.created
    p.source   = rec.source or "local"
    p.readonly = false
    p.rowClues, p.colClues = Clues.derive(p.solution, p.w, p.h)
    return p
end

-- pc.sys exposes a clock but no strftime, and os.* is blocked in the sandbox, so
-- a plain date string is assembled by hand. Only used for display ordering.
local function today()
    local ok, c = pcall(pc.sys.getClock)
    if ok and type(c) == "table" and c.year then
        return ("%04d-%02d-%02d"):format(c.year, c.month or 1, c.day or 1)
    end
    return ""
end

-- Derives a stable id from the code so importing the same puzzle twice does not
-- create a duplicate.
local function idFor(code)
    local body = code:gsub(":.*$", "")
    local h = 0x811C9DC5
    for i = 1, #body do
        h = h ~ body:byte(i)
        h = (h * 16777619) & 0xFFFFFFFF
    end
    return ("p_%08x"):format(h)
end

function ST.save(puzzle)
    if not ST.available() then return nil, "picocalc.json unavailable" end

    local code = Share.encode({ w = puzzle.w, h = puzzle.h,
                                solution = puzzle.solution })
    if not code then return nil, "cannot encode puzzle" end

    local id = puzzle.id or idFor(code)
    local rec = {
        v       = 1,
        id      = id,
        name    = puzzle.name or "UNTITLED",
        author  = puzzle.author or "",
        w       = puzzle.w,
        h       = puzzle.h,
        code    = code,
        source  = puzzle.source or "local",
        created = puzzle.created or today(),
    }

    local enc = json().encode(rec, { indent = 1 })
    local path = ST.dir() .. "/" .. id .. ".json"
    local fh = fs.open(path, "w")
    if not fh then return nil, "cannot write " .. path end
    fs.write(fh, enc)
    fs.close(fh)
    return id
end

function ST.load(id)
    if not ST.available() then return nil, "picocalc.json unavailable" end
    local path = ST.dir() .. "/" .. id .. ".json"
    local src = fs.readFile(path)
    if not src then return nil, "not found" end
    local rec, err = json().decode(src)
    if not rec then return nil, err end
    return hydrate(rec)
end

function ST.list()
    if not ST.available() then return {} end
    local out = {}
    local entries = fs.glob(ST.dir(), "*.json")
    for _, e in ipairs(entries or {}) do
        if not e.is_dir then
            local src = fs.readFile(ST.dir() .. "/" .. e.name)
            if src then
                local rec = json().decode(src)
                -- Only the summary is kept in the list; the grid is decoded
                -- lazily on open, so a big library does not cost memory.
                if rec and rec.id then
                    out[#out + 1] = {
                        id = rec.id, name = rec.name or "UNTITLED",
                        w = rec.w, h = rec.h, source = rec.source,
                    }
                end
            end
        end
    end
    table.sort(out, function(a, b) return (a.name or "") < (b.name or "") end)
    return out
end

function ST.delete(id)
    local path = ST.dir() .. "/" .. id .. ".json"
    if not fs.exists(path) then return false end
    return fs.delete(path)
end

-- Imports a typed or pasted share code. Returns id, puzzle or nil, err.
function ST.importCode(code, name)
    local p, err = Share.decode(code)
    if not p then return nil, err end
    p.name = name or p.name or "IMPORTED"
    p.source = "share"
    local id, serr = ST.save(p)
    if not id then return nil, serr end
    p.id = id
    p.rowClues, p.colClues = Clues.derive(p.solution, p.w, p.h)
    return id, p
end

function ST.exportCode(puzzle)
    return Share.encode({ w = puzzle.w, h = puzzle.h,
                          solution = puzzle.solution, name = puzzle.name })
end

return ST
