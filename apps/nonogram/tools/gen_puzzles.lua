-- gen_puzzles.lua — host-side puzzle generator.
--
--   lua apps/nonogram/tools/gen_puzzles.lua [count-per-size] > apps/nonogram/generated.lua
--
-- Deliberately reuses the SAME solver.lua and clues.lua the game ships, rather
-- than reimplementing the check here. Anything this emits is therefore
-- guaranteed solvable by the app itself — and it cannot drift, because there is
-- only one solver.
--
-- Only puzzles verified "unique-line" are kept: exactly one solution, reachable
-- by pure line logic with no guessing. That is the property that makes a
-- nonogram fair. Random grids frequently fail it (a 2x2 switch anywhere makes
-- the clues ambiguous), so the generator over-produces and filters.

local here = arg[0]:match("(.*)/tools/gen_puzzles%.lua$") or "apps/nonogram"

local _loaded = {}
function require(name)
    if _loaded[name] then return _loaded[name] end
    local path = here .. "/" .. name:gsub("%.", "/") .. ".lua"
    local f = assert(io.open(path, "r"), "cannot open " .. path)
    local src = f:read("a"); f:close()
    local r = assert(load(src, "@" .. path))()
    if r == nil then r = true end
    _loaded[name] = r
    return r
end

local Clues = require("clues")
local Solver = require("solver")

local PER_SIZE = tonumber(arg[1]) or 14
local SIZES = { 5, 10, 15, 20 }

-- ── Candidate generation ─────────────────────────────────────────────────────

local function blank(n)
    local g = {}
    for r = 1, n do
        g[r] = {}
        for c = 1, n do g[r][c] = 0 end
    end
    return g
end

local function noise(n, density)
    local g = blank(n)
    for r = 1, n do
        for c = 1, n do
            g[r][c] = (math.random() < density) and 1 or 0
        end
    end
    return g
end

-- Majority-rule smoothing (the classic cave-generation step). Turns speckle into
-- organic blobs, which both look intentional and tend to be MORE constrained —
-- long runs give the line solver more to work with, so smoothed grids pass the
-- uniqueness filter far more often than raw noise.
local function smooth(g, n)
    local out = blank(n)
    for r = 1, n do
        for c = 1, n do
            local live = 0
            for dr = -1, 1 do
                for dc = -1, 1 do
                    local rr, cc = r + dr, c + dc
                    if rr >= 1 and rr <= n and cc >= 1 and cc <= n then
                        live = live + g[rr][cc]
                    else
                        live = live + g[r][c]   -- treat edges as self, avoids rims
                    end
                end
            end
            out[r][c] = (live >= 5) and 1 or 0
        end
    end
    return out
end

-- Removes fully empty rows/columns by nudging a cell on, and clamps runaway
-- density. An all-empty line is legal (clue {0}) but a grid full of them reads
-- as a mistake.
local function tidy(g, n)
    for r = 1, n do
        local any = false
        for c = 1, n do if g[r][c] == 1 then any = true break end end
        if not any then g[r][math.random(n)] = 1 end
    end
    for c = 1, n do
        local any = false
        for r = 1, n do if g[r][c] == 1 then any = true break end end
        if not any then g[math.random(n)][c] = 1 end
    end
    return g
end

local function density(g, n)
    local f = 0
    for r = 1, n do for c = 1, n do f = f + g[r][c] end end
    return f / (n * n)
end

local function candidate(n)
    local d = 0.42 + math.random() * 0.22          -- 0.42 - 0.64
    local g = noise(n, d)
    local passes = (n <= 5) and 1 or 2
    for _ = 1, passes do g = smooth(g, n) end
    return tidy(g, n)
end

local function gridKey(g, n)
    local parts = {}
    for r = 1, n do
        local row = {}
        for c = 1, n do row[c] = g[r][c] end
        parts[r] = table.concat(row)
    end
    return table.concat(parts, "/")
end

-- ── Names ────────────────────────────────────────────────────────────────────
-- Deterministic cyberpunk-flavoured pairings, so a regenerated set with the same
-- seed produces the same names.

local A = {
    "NEON", "CHROME", "GHOST", "BLACK", "RUST", "VOID", "HEX", "ZERO", "IRON",
    "CIPHER", "STATIC", "VAPOR", "COBALT", "ACID", "GLASS", "NIGHT", "PULSE",
    "ASH", "SILK", "OZONE", "CINDER", "QUARTZ", "SABLE", "FLUX",
}
local B = {
    "WIRE", "DRIFT", "GATE", "SHARD", "CORE", "MESH", "LOCK", "SPIKE", "VEIL",
    "RELAY", "BLOOM", "SPINE", "HALO", "DRONE", "TRACE", "SIGIL", "WARD",
    "CRAWL", "SHELL", "ECHO", "BRAND", "SPUR", "GRAFT", "SEAM",
}

local used = {}

-- Both words advance on every step, using a stride coprime with #B so the
-- second word cycles through all of its options rather than changing only once
-- per pass over the first list. Without this, a whole size band came out as
-- "... CORE" / "... MESH" and the set read as lazily generated.
local function nameFor(i)
    for attempt = 0, #A * #B do
        local k = i + attempt
        local nm = A[(k % #A) + 1] .. " " .. B[((k * 7) % #B) + 1]
        if not used[nm] then used[nm] = true; return nm end
    end
    return "GRID " .. i
end

-- ── Main ─────────────────────────────────────────────────────────────────────

-- Fixed seed: the emitted file is checked in, so generation must be
-- reproducible. Lua 5.4's randomseed(n) is deterministic for a given n.
math.randomseed(20260725)

local out = {}
local stats = {}
local seen = {}

for _, n in ipairs(SIZES) do
    local kept, tried, verdicts = 0, 0, {}
    -- Cap attempts so a hostile size cannot spin forever.
    local maxTries = PER_SIZE * 120

    while kept < PER_SIZE and tried < maxTries do
        tried = tried + 1
        local g = candidate(n)

        local key = gridKey(g, n)
        if not seen[key] then
            local d = density(g, n)
            -- Reject the degenerate extremes: near-empty and near-solid grids are
            -- trivially solvable and read as broken.
            if d >= 0.30 and d <= 0.75 then
                local rc, cc = Clues.derive(g, n, n)
                local v = Solver.check(n, n, rc, cc, 30000)
                verdicts[v] = (verdicts[v] or 0) + 1
                if v == "unique-line" then
                    seen[key] = true
                    kept = kept + 1
                    out[#out + 1] = { n = n, g = g }
                end
            end
        end
    end

    stats[#stats + 1] = { n = n, kept = kept, tried = tried, verdicts = verdicts }
end

-- ── Emit ─────────────────────────────────────────────────────────────────────

local function emit(s) io.write(s) end

emit("-- generated.lua — procedurally generated puzzles.\n")
emit("--\n")
emit("-- DO NOT EDIT BY HAND. Regenerate with:\n")
emit("--   lua apps/nonogram/tools/gen_puzzles.lua > apps/nonogram/generated.lua\n")
emit("--\n")
emit("-- Every grid here was verified \"unique-line\" by solver.lua at generation\n")
emit("-- time: exactly one solution, reachable by pure line logic without\n")
emit("-- guessing. tests/run_host.lua re-checks all of them on every run, so a\n")
emit("-- regression in the solver or a bad edit here fails the suite.\n")
emit("\nreturn {\n")

for idx, p in ipairs(out) do
    local nm = nameFor(idx)
    emit(("    { name = %q, size = %d, rows = {\n"):format(nm, p.n))
    for r = 1, p.n do
        local row = {}
        for c = 1, p.n do row[c] = (p.g[r][c] == 1) and "#" or "." end
        emit(('        "%s",\n'):format(table.concat(row)))
    end
    emit("    } },\n")
end

emit("}\n")

-- Stats to stderr so stdout stays a clean Lua module.
io.stderr:write("\ngeneration summary\n")
for _, s in ipairs(stats) do
    local vs = {}
    for k, v in pairs(s.verdicts) do vs[#vs + 1] = k .. "=" .. v end
    table.sort(vs)
    io.stderr:write(("  %2dx%-2d kept %2d of %4d tried   [%s]\n")
        :format(s.n, s.n, s.kept, s.tried, table.concat(vs, " ")))
end
io.stderr:write(("  total kept: %d\n"):format(#out))
