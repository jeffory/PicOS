-- solver.lua — line solver plus a bounded uniqueness check for create mode.
-- Pure logic, no picocalc.*, no drawing.
--
-- Two deliberate implementation constraints:
--
--  * NO coroutines. lua_bridge.c registers only _G, table, string and math, so
--    the coroutine library is absent on device (CLAUDE.md lists it as available,
--    but luaL_requiref is never called for it). Stepping is therefore explicit:
--    job:step(budget) does a bounded amount of work and returns.
--
--  * NO recursion over cells. LUAI_MAXSTACK is 500, so a depth-first search
--    recursing once per cell would blow the Lua stack on a 15x15 (225 cells).
--    The search keeps its own stack in a table.
--
-- The line solver is dynamic programming, not enumeration. Enumerating
-- placements is C(free+k, k) — 3003 for a 15-line but 77520 for a 20-line, which
-- is hopeless in Lua. The DP is O(n*k).

local S = {}

local EMPTY, FILLED, BLOCKED = 0, 1, 2

S.UNKNOWN = EMPTY
S.FILLED  = FILLED
S.BLOCKED = BLOCKED

-- ── Line solver ──────────────────────────────────────────────────────────────
--
-- `line` is an array of n values in {EMPTY, FILLED, BLOCKED} where EMPTY means
-- undecided. Returns:
--   changed (bool), or
--   nil, "contradiction"
--
-- On success `line` is mutated in place with every cell the clues force.

-- can[j][i] = clues j..k can validly fill cells i..n
local function buildSuffix(line, clues, n, k)
    local can = {}
    for j = 1, k + 2 do can[j] = {} end

    -- With no clues left, the rest of the line must contain no FILLED.
    can[k + 1][n + 1] = true
    for i = n, 1, -1 do
        can[k + 1][i] = (line[i] ~= FILLED) and can[k + 1][i + 1] or false
    end

    for j = k, 1, -1 do
        local len = clues[j]
        can[j][n + 2] = false
        can[j][n + 1] = false
        for i = n, 1, -1 do
            local ok = false

            -- Option A: leave cell i blank and defer clue j.
            if line[i] ~= FILLED and can[j][i + 1] then ok = true end

            -- Option B: place clue j starting at i.
            if not ok and i + len - 1 <= n then
                local fits = true
                for t = i, i + len - 1 do
                    if line[t] == BLOCKED then fits = false; break end
                end
                if fits then
                    local after = i + len
                    if after <= n and line[after] == FILLED then
                        fits = false          -- run would be too long
                    end
                    if fits then
                        local nextI = after + 1
                        if nextI > n + 1 then nextI = n + 1 end
                        if can[j + 1][nextI] then ok = true end
                    end
                end
            end

            can[j][i] = ok
        end
    end
    return can
end

function S.solveLine(line, clues, n)
    -- A {0} clue list means "no filled cells at all".
    local k = #clues
    if k == 1 and clues[1] == 0 then k = 0 end

    -- suf[j][i] : clues j..k fit in cells i..n
    local suf = buildSuffix(line, clues, n, k)
    if not suf[1][1] then return nil, "contradiction" end

    -- pre[j][i] : clues 1..j-1 fit in cells 1..i-1.
    --
    -- Rather than write a second mirrored recurrence (and a second place for an
    -- off-by-one to hide), run the SAME suffix builder over the reversed line
    -- and reversed clue list, then index it backwards:
    --   forward clue j-1  <->  reversed clue k-j+2
    --   forward cell  i-1 <->  reversed cell  n-i+2
    local lineR, cluesR = {}, {}
    for i = 1, n do lineR[i] = line[n - i + 1] end
    for j = 1, k do cluesR[j] = clues[k - j + 1] end
    local sufR = buildSuffix(lineR, cluesR, n, k)

    local function pre(j, i)
        local m = k - j + 2
        local p = n - i + 2
        local row = sufR[m]
        return row and row[p] or false
    end

    local canFill, canBlank = {}, {}
    for i = 1, n do canFill[i], canBlank[i] = false, false end

    -- A cell can be blank when some split point j leaves clues 1..j-1 entirely
    -- before it and clues j..k entirely after it.
    for i = 1, n do
        if line[i] ~= FILLED then
            for j = 1, k + 1 do
                if pre(j, i) and suf[j][i + 1] then
                    canBlank[i] = true
                    break
                end
            end
        end
    end

    -- Can clue j legally START at cell p?
    --
    -- Not just "clues 1..j-1 fit before p": cell p-1 must also be blank, or the
    -- new run would abut the previous one and merge into a single longer run.
    -- Without this, {1,1,1} in a 5-line reported that cell 2 could be filled
    -- (clue 1 at cell 1, clue 2 at cell 2 — adjacent), so cells 2 and 4 were
    -- never deduced as blocked.
    local function canStartAt(j, p)
        if p == 1 then return pre(j, 1) end
        if line[p - 1] == FILLED then return false end
        return pre(j, p - 1)
    end

    -- A cell can be filled when some valid placement of some clue covers it.
    for j = 1, k do
        local len = clues[j]
        for p = 1, n - len + 1 do
            if canStartAt(j, p) then
                local fits = true
                for t = p, p + len - 1 do
                    if line[t] == BLOCKED then fits = false; break end
                end
                if fits then
                    local after = p + len
                    if after <= n and line[after] == FILLED then fits = false end
                    if fits then
                        local nextI = (after <= n) and (after + 1) or (n + 1)
                        if suf[j + 1][nextI] then
                            for t = p, p + len - 1 do canFill[t] = true end
                        end
                    end
                end
            end
        end
    end

    local changed = false
    for i = 1, n do
        if line[i] == EMPTY then
            if canFill[i] and not canBlank[i] then
                line[i] = FILLED; changed = true
            elseif canBlank[i] and not canFill[i] then
                line[i] = BLOCKED; changed = true
            elseif not canBlank[i] and not canFill[i] then
                return nil, "contradiction"
            end
        end
    end

    return changed
end

-- ── Whole-grid propagation and uniqueness ────────────────────────────────────

local function copyGrid(g, w, h)
    local out = {}
    for r = 1, h do
        local src, dst = g[r], {}
        for c = 1, w do dst[c] = src[c] end
        out[r] = dst
    end
    return out
end

local function getRow(grid, r, w)
    local line = {}
    for c = 1, w do line[c] = grid[r][c] end
    return line
end

local function putRow(grid, r, w, line)
    for c = 1, w do grid[r][c] = line[c] end
end

local function getCol(grid, c, h)
    local line = {}
    for r = 1, h do line[r] = grid[r][c] end
    return line
end

local function putCol(grid, c, h, line)
    for r = 1, h do grid[r][c] = line[r] end
end

-- Runs line-solving to a fixpoint. Returns "solved" | "stuck" | "contradiction".
local function propagate(grid, w, h, rowClues, colClues, budget)
    local passes = 0
    repeat
        local changed = false
        passes = passes + 1

        for r = 1, h do
            local line = getRow(grid, r, w)
            local ch, err = S.solveLine(line, rowClues[r], w)
            if ch == nil then return "contradiction", passes end
            if ch then putRow(grid, r, w, line); changed = true end
        end

        for c = 1, w do
            local line = getCol(grid, c, h)
            local ch, err = S.solveLine(line, colClues[c], h)
            if ch == nil then return "contradiction", passes end
            if ch then putCol(grid, c, h, line); changed = true end
        end

        if budget and passes >= budget then break end
    until not changed

    for r = 1, h do
        for c = 1, w do
            if grid[r][c] == EMPTY then return "stuck", passes end
        end
    end
    return "solved", passes
end

S.propagate = propagate

-- Creates an incremental uniqueness-check job.
--
-- job:step(budget) -> "running" | "done"; when done, job.result is one of:
--   "unique-line"    solvable by pure line logic — the good case
--   "guess"          exactly one solution, but it needs a guess to find
--   "multi"          two or more solutions: the clues are ambiguous
--   "contradiction"  no solution (only reachable if the clues are inconsistent)
--   "timeout"        node budget exhausted before deciding
function S.newJob(w, h, rowClues, colClues, maxNodes)
    local job = {
        w = w, h = h, rowClues = rowClues, colClues = colClues,
        maxNodes = maxNodes or 4000,
        nodes = 0,
        solutions = 0,
        result = nil,
        progress = 0,
        phase = "propagate",
    }

    local blank = {}
    for r = 1, h do
        blank[r] = {}
        for c = 1, w do blank[r][c] = EMPTY end
    end
    job.stack = { blank }

    function job:step(budget)
        budget = budget or 40

        if self.result then return "done" end

        for _ = 1, budget do
            local grid = table.remove(self.stack)
            if not grid then
                -- Search exhausted.
                if self.solutions == 0 then
                    self.result = "contradiction"
                elseif self.solutions == 1 then
                    self.result = (self.phase == "propagate")
                        and "unique-line" or "guess"
                else
                    self.result = "multi"
                end
                return "done"
            end

            self.nodes = self.nodes + 1
            if self.nodes > self.maxNodes then
                self.result = "timeout"
                return "done"
            end
            self.progress = self.nodes / self.maxNodes

            local status = propagate(grid, self.w, self.h,
                                     self.rowClues, self.colClues)

            if status == "solved" then
                self.solutions = self.solutions + 1
                -- Two solutions is all it takes to prove ambiguity; there is no
                -- value in finding a third.
                if self.solutions >= 2 then
                    self.result = "multi"
                    return "done"
                end
            elseif status == "stuck" then
                -- Branch on the first undecided cell. Reaching here at all means
                -- pure line logic was not enough.
                self.phase = "search"
                local bc, br
                for r = 1, self.h do
                    for c = 1, self.w do
                        if grid[r][c] == EMPTY then bc, br = c, r; break end
                    end
                    if bc then break end
                end
                if bc then
                    local a = copyGrid(grid, self.w, self.h)
                    a[br][bc] = FILLED
                    local b = copyGrid(grid, self.w, self.h)
                    b[br][bc] = BLOCKED
                    self.stack[#self.stack + 1] = a
                    self.stack[#self.stack + 1] = b
                end
            end
            -- "contradiction" just prunes this branch.
        end

        return "running"
    end

    return job
end

-- Convenience wrapper for host tests: runs a job to completion.
function S.check(w, h, rowClues, colClues, maxNodes)
    local job = S.newJob(w, h, rowClues, colClues, maxNodes)
    local guard = 0
    while job:step(200) == "running" do
        guard = guard + 1
        if guard > 10000 then return "timeout" end
    end
    return job.result
end

return S
