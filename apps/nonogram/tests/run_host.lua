-- Host test runner for the pure-logic modules.
--
--   lua apps/nonogram/tests/run_host.lua
--
-- These modules deliberately have no picocalc.* dependency, which is what lets
-- them be tested here in milliseconds instead of via simulator round-trips.
-- The same files also load on the device, so a host pass is not a substitute for
-- the E2E run — it just catches logic errors far cheaper.

local here = arg[0]:match("(.*)/tests/run_host%.lua$") or "apps/nonogram"

-- Stand-in for the app's require shim (require/dofile are blocked on device).
local _loaded = {}
function require(name)
    if _loaded[name] then return _loaded[name] end
    local path = here .. "/" .. name:gsub("%.", "/") .. ".lua"
    local f = assert(io.open(path, "r"), "cannot open " .. path)
    local src = f:read("a")
    f:close()
    local fn = assert(load(src, "@" .. path))
    local r = fn()
    if r == nil then r = true end
    _loaded[name] = r
    return r
end

local Clues = require("clues")
local Layout = require("layout")
local Board = require("board")
local Share = require("sharecode")

local pass, fail = 0, 0
local function ok(name, cond, detail)
    if cond then
        pass = pass + 1
    else
        fail = fail + 1
        print(("FAIL  %s  %s"):format(name, tostring(detail or "")))
    end
end

local function eqList(a, b)
    if #a ~= #b then return false end
    for i = 1, #a do if a[i] ~= b[i] then return false end end
    return true
end

local function listStr(t)
    local p = {}
    for i = 1, #t do p[i] = tostring(t[i]) end
    return "{" .. table.concat(p, ",") .. "}"
end

-- ── clues ────────────────────────────────────────────────────────────────────

local function lineOf(t)
    return function(i) return t[i] end
end

ok("clues_simple",
   eqList(Clues.fromLine(lineOf({1,1,0,1,0}), 5), {2,1}),
   listStr(Clues.fromLine(lineOf({1,1,0,1,0}), 5)))
ok("clues_full", eqList(Clues.fromLine(lineOf({1,1,1}), 3), {3}))
ok("clues_empty_is_zero", eqList(Clues.fromLine(lineOf({0,0,0}), 3), {0}))
ok("clues_blocked_not_filled",
   eqList(Clues.fromLine(lineOf({1,2,1}), 3), {1,1}),
   listStr(Clues.fromLine(lineOf({1,2,1}), 3)))
ok("clues_maybe_not_filled",
   eqList(Clues.fromLine(lineOf({1,3,1}), 3), {1,1}))
ok("clues_edges", eqList(Clues.fromLine(lineOf({1,0,0,0,1}), 5), {1,1}))

-- derive against a hand-checked 5x5 (a plus sign)
local plus = {
    {0,0,1,0,0},
    {0,0,1,0,0},
    {1,1,1,1,1},
    {0,0,1,0,0},
    {0,0,1,0,0},
}
local rc, cc = Clues.derive(plus, 5, 5)
ok("derive_rows", eqList(rc[1], {1}) and eqList(rc[3], {5}) and eqList(rc[5], {1}))
ok("derive_cols", eqList(cc[1], {1}) and eqList(cc[3], {5}) and eqList(cc[5], {1}))
ok("maxClueCount_5", Clues.maxClueCount(5) == 3, Clues.maxClueCount(5))
ok("maxClueCount_20", Clues.maxClueCount(20) == 10, Clues.maxClueCount(20))

-- classify
ok("classify_neutral_empty",
   Clues.classify(lineOf({0,0,0,0,0}), 5, {2,1}) == "neutral")
ok("classify_done",
   Clues.classify(lineOf({1,1,0,1,0}), 5, {2,1}) == "done",
   Clues.classify(lineOf({1,1,0,1,0}), 5, {2,1}))
ok("classify_over_count",
   Clues.classify(lineOf({1,1,1,1,1}), 5, {2,1}) == "over")
ok("classify_over_run_too_long",
   Clues.classify(lineOf({1,1,1,0,0}), 5, {2,1}) == "over",
   Clues.classify(lineOf({1,1,1,0,0}), 5, {2,1}))
-- Right count, wrong arrangement must stay neutral: signalling it would leak
-- information the player has not earned.
ok("classify_right_count_wrong_place_is_neutral",
   Clues.classify(lineOf({1,0,1,0,1}), 5, {2,1}) == "neutral",
   Clues.classify(lineOf({1,0,1,0,1}), 5, {2,1}))
ok("classify_done_with_blocks",
   Clues.classify(lineOf({1,1,2,1,2}), 5, {2,1}) == "done",
   Clues.classify(lineOf({1,1,2,1,2}), 5, {2,1}))

-- ── layout ───────────────────────────────────────────────────────────────────

local SIZES = { {5,5}, {10,10}, {15,15}, {20,20} }
for _, sz in ipairs(SIZES) do
    local w, h = sz[1], sz[2]
    local L = Layout.compute(w, h)
    local tag = w .. "x" .. h

    ok("layout_" .. tag .. "_cell_at_least_12", L.cell >= 12,
       "cell=" .. L.cell)
    ok("layout_" .. tag .. "_fits_width", L.gridRight <= 320,
       "gridRight=" .. L.gridRight)
    ok("layout_" .. tag .. "_fits_height", L.gridBottom <= 320 - 20,
       "gridBottom=" .. L.gridBottom)
    ok("layout_" .. tag .. "_gutter_on_screen", L.gutterX >= 0,
       "gutterX=" .. L.gutterX)
    ok("layout_" .. tag .. "_viewport_within_board",
       L.vcols <= w and L.vrows <= h,
       ("v=%dx%d"):format(L.vcols, L.vrows))
end

-- 5x5, 10x10 and 15x15 must fit whole; 20x20 is expected to scroll because 20
-- rows of column clues leave under 12px per row.
for _, sz in ipairs({ {5,5}, {10,10}, {15,15} }) do
    local L = Layout.compute(sz[1], sz[2])
    ok("layout_" .. sz[1] .. "_no_scroll", not L.scrolls,
       ("v=%dx%d cell=%d"):format(L.vcols, L.vrows, L.cell))
end
do
    local L = Layout.compute(20, 20)
    ok("layout_20_scrolls", L.scrolls,
       ("v=%dx%d cell=%d"):format(L.vcols, L.vrows, L.cell))
end

-- Actual clue lists should give a tighter gutter than the worst case, and so a
-- cell at least as large.
do
    local rcs, ccs = Clues.derive(plus, 5, 5)
    local worst = Layout.compute(5, 5)
    local tight = Layout.compute(5, 5, rcs, ccs)
    ok("layout_actual_clues_no_worse",
       tight.cell >= worst.cell and tight.gutterW <= worst.gutterW,
       ("tight cell=%d gut=%d / worst cell=%d gut=%d")
           :format(tight.cell, tight.gutterW, worst.cell, worst.gutterW))
end

-- cellRect / viewport
do
    local L = Layout.compute(20, 20)
    ok("cellRect_visible_origin", L:cellRect(1, 1) ~= nil)
    local offC = L.vcols + 1
    ok("cellRect_offscreen_is_nil", L:cellRect(offC, 1) == nil,
       "vcols=" .. L.vcols)
    L:ensureVisible(20, 20)
    ok("ensureVisible_scrolls_to_corner", L:cellRect(20, 20) ~= nil,
       ("ox=%d oy=%d"):format(L.ox, L.oy))
    ok("ensureVisible_clamped", L.ox <= 20 - L.vcols and L.oy <= 20 - L.vrows)
    L:ensureVisible(1, 1)
    ok("ensureVisible_back_to_origin", L.ox == 0 and L.oy == 0,
       ("ox=%d oy=%d"):format(L.ox, L.oy))
end

-- ── board ────────────────────────────────────────────────────────────────────

local function newPlusBoard()
    local rcs, ccs = Clues.derive(plus, 5, 5)
    return Board.new({ w = 5, h = 5, rowClues = rcs, colClues = ccs })
end

do
    local b = newPlusBoard()
    ok("board_starts_empty", b:get(1, 1) == Board.EMPTY)
    ok("board_not_solved_initially", not b:isSolved())

    b:toggleFilled(3, 1)
    ok("board_toggle_fills", b:get(3, 1) == Board.FILLED)
    ok("board_row_done_after_correct_fill", b.rowState[1] == "done",
       b.rowState[1])
    b:toggleFilled(3, 1)
    ok("board_toggle_clears", b:get(3, 1) == Board.EMPTY)

    b:toggleBlocked(1, 1)
    ok("board_blocked", b:get(1, 1) == Board.BLOCKED)
    b:toggleMaybe(1, 1)
    ok("board_maybe_overrides_blocked", b:get(1, 1) == Board.MAYBE)

    -- undo / redo
    local before = b:get(1, 1)
    b:undo()
    ok("board_undo_restores", b:get(1, 1) ~= before)
    b:redo()
    ok("board_redo_reapplies", b:get(1, 1) == before)
end

-- Solving the whole plus must reach isSolved.
do
    local b = newPlusBoard()
    for r = 1, 5 do
        for c = 1, 5 do
            if plus[r][c] == 1 then b:set(c, r, Board.FILLED) end
        end
    end
    ok("board_solved_on_correct_grid", b:isSolved())
    ok("board_no_errors_when_solved", not b:hasErrors())
end

-- Over-fill must be reported.
do
    local b = newPlusBoard()
    for c = 1, 5 do b:set(c, 1, Board.FILLED) end   -- row 1 wants {1}
    ok("board_reports_overfill", b.rowState[1] == "over", b.rowState[1])
    ok("board_hasErrors", b:hasErrors())
end

-- Grouped edits undo as one action.
do
    local b = newPlusBoard()
    b:beginGroup()
    b:set(1, 2, Board.BLOCKED)
    b:set(2, 2, Board.BLOCKED)
    b:set(4, 2, Board.BLOCKED)
    b:endGroup()
    ok("board_group_applied", b:get(1, 2) == Board.BLOCKED
        and b:get(4, 2) == Board.BLOCKED)
    b:undo()
    ok("board_group_undoes_together",
       b:get(1, 2) == Board.EMPTY and b:get(2, 2) == Board.EMPTY
       and b:get(4, 2) == Board.EMPTY)
end

-- autoX blocks the rest of a satisfied line.
do
    local b = newPlusBoard()
    b.autoX = true
    b:set(3, 1, Board.FILLED)     -- row 1 clue is {1}: now satisfied
    local blocked = 0
    for c = 1, 5 do
        if b:get(c, 1) == Board.BLOCKED then blocked = blocked + 1 end
    end
    ok("autoX_fills_rest_of_row", blocked == 4, "blocked=" .. blocked)
    b:undo()
    local stillBlocked = 0
    for c = 1, 5 do
        if b:get(c, 1) == Board.BLOCKED then stillBlocked = stillBlocked + 1 end
    end
    ok("autoX_undoes_with_its_edit", stillBlocked == 0,
       "blocked=" .. stillBlocked)
end

-- snapshot / restore
do
    local b = newPlusBoard()
    b:set(1, 1, Board.FILLED); b:set(2, 2, Board.BLOCKED); b:set(3, 3, Board.MAYBE)
    local snap = b:snapshot()
    local b2 = newPlusBoard()
    ok("board_restore", b2:restore(snap)
        and b2:get(1, 1) == Board.FILLED
        and b2:get(2, 2) == Board.BLOCKED
        and b2:get(3, 3) == Board.MAYBE)
    ok("board_restore_rejects_wrong_size", not b2:restore({ 1, 2, 3 }))
end

-- ── sharecode ────────────────────────────────────────────────────────────────

local function randGrid(w, h, seed)
    math.randomseed(seed)
    local g = {}
    for r = 1, h do
        g[r] = {}
        for c = 1, w do g[r][c] = (math.random() < 0.5) and 1 or 0 end
    end
    return g
end

local function gridsEqual(a, b, w, h)
    for r = 1, h do
        for c = 1, w do
            if a[r][c] ~= b[r][c] then return false end
        end
    end
    return true
end

-- Length budget. The 127-char cap is a ui.textInput limit, so it only binds on
-- codes a player types by hand. The four sizes the game OFFERS must fit; larger
-- encodings are still valid for file and API transfer, and are only expected to
-- fail the typability check.
local OFFERED = { {5,5}, {10,10}, {15,15}, {20,20} }
local TOO_BIG_TO_TYPE = { {25,25} }

print()
print("share code lengths (ui.textInput cap = 127, with a 9-char title):")
for _, sz in ipairs({ {5,5}, {10,10}, {15,15}, {20,20}, {25,25} }) do
    local w, h = sz[1], sz[2]
    local g = randGrid(w, h, w * 100 + h)
    local code = Share.encode({ w = w, h = h, solution = g, name = "TEST NAME" })
    local grouped = Share.group(code, 10)
    print(("  %-7s raw=%-4d grouped=%-4d  %s")
        :format(w .. "x" .. h, #code, #grouped,
                Share.fitsTextInput(grouped) and "typable" or "file/API only"))
end
print()

for _, sz in ipairs(OFFERED) do
    local w, h = sz[1], sz[2]
    local g = randGrid(w, h, w * 100 + h)
    local grouped = Share.group(
        Share.encode({ w = w, h = h, solution = g, name = "TEST NAME" }), 10)
    ok("share_typable_" .. w .. "x" .. h, Share.fitsTextInput(grouped),
       "grouped=" .. #grouped)
end

-- Oversized codes must still encode and round-trip; they are just not typable.
for _, sz in ipairs(TOO_BIG_TO_TYPE) do
    local w, h = sz[1], sz[2]
    local g = randGrid(w, h, 11)
    local code = Share.encode({ w = w, h = h, solution = g })
    local p = Share.decode(code)
    ok("share_oversize_still_roundtrips_" .. w .. "x" .. h,
       p ~= nil and gridsEqual(g, p.solution, w, h))
    ok("share_oversize_flagged_untypable_" .. w .. "x" .. h,
       not Share.fitsTextInput(code), "#code=" .. #code)
end

-- Round-trip at every supported size, several random grids each.
for _, sz in ipairs({ {5,5}, {10,10}, {15,15}, {20,20}, {12,12}, {10,15}, {15,10} }) do
    local w, h = sz[1], sz[2]
    local allOk = true
    local detail = nil
    for seed = 1, 25 do
        local g = randGrid(w, h, seed * 7919 + w)
        local code = Share.encode({ w = w, h = h, solution = g })
        local p, err = Share.decode(code)
        if not p then allOk = false; detail = "decode: " .. tostring(err); break end
        if p.w ~= w or p.h ~= h then
            allOk = false; detail = ("size %dx%d"):format(p.w, p.h); break
        end
        if not gridsEqual(g, p.solution, w, h) then
            allOk = false; detail = "grid mismatch seed=" .. seed; break
        end
    end
    ok("share_roundtrip_" .. w .. "x" .. h, allOk, detail)
end

-- Escape form for a size not in the table.
do
    local g = randGrid(7, 9, 42)
    local code = Share.encode({ w = 7, h = 9, solution = g })
    local p, err = Share.decode(code)
    ok("share_escape_size", p ~= nil and p.w == 7 and p.h == 9
        and gridsEqual(g, p.solution, 7, 9), tostring(err))
end

-- Title survives, including one containing dashes and spaces.
do
    local g = randGrid(5, 5, 1)
    local code = Share.encode({ w = 5, h = 5, solution = g,
                                name = "NEON-CAT 2 - final" })
    local p = Share.decode(code)
    ok("share_title_with_dashes", p ~= nil and p.name == "NEON-CAT 2 - final",
       p and p.name)
end

-- Grouped codes must decode identically to raw ones. This is the bug the ':'
-- title separator exists to prevent: with '-' for both, a grouped code parses
-- as body + title.
do
    local g = randGrid(15, 15, 3)
    local code = Share.encode({ w = 15, h = 15, solution = g, name = "GRID" })
    local grouped = Share.group(code, 10)
    local a, b = Share.decode(code), Share.decode(grouped)
    ok("share_grouped_decodes", a ~= nil and b ~= nil
        and gridsEqual(a.solution, b.solution, 15, 15)
        and a.name == b.name,
       "grouped=" .. grouped)
end

-- Case-insensitive and whitespace-tolerant entry.
do
    local g = randGrid(10, 10, 5)
    local code = Share.encode({ w = 10, h = 10, solution = g })
    local messy = code:lower():gsub("(...)", "%1 ")
    local p = Share.decode(messy)
    ok("share_case_and_space_tolerant",
       p ~= nil and gridsEqual(g, p.solution, 10, 10), messy)
end

-- Corruption must be caught, not silently decoded into a different puzzle.
do
    local g = randGrid(10, 10, 9)
    local code = Share.encode({ w = 10, h = 10, solution = g })
    local caught, total = 0, 0
    for i = 3, #code - 2 do
        local ch = code:sub(i, i)
        local sub = (ch == "0") and "1" or "0"
        local bad = code:sub(1, i - 1) .. sub .. code:sub(i + 1)
        if bad ~= code then
            total = total + 1
            local p = Share.decode(bad)
            if p == nil or not gridsEqual(g, p.solution, 10, 10) then
                caught = caught + 1
            end
        end
    end
    ok("share_detects_single_char_corruption", caught == total,
       ("caught %d/%d"):format(caught, total))
end

-- Garbage must fail softly.
do
    local bad = { "", "X", "N2AAAA", "hello world", "N1", "----", "N1Z" }
    local allSoft = true
    local which = nil
    for _, s in ipairs(bad) do
        local okc, p, e = pcall(Share.decode, s)
        if not okc then allSoft = false; which = "raised on " .. s; break end
        if p ~= nil then allSoft = false; which = "accepted " .. s; break end
        if type(e) ~= "string" then allSoft = false; which = "no err for " .. s; break end
    end
    ok("share_garbage_soft_fail", allSoft, which)
end

-- ── solver ───────────────────────────────────────────────────────────────────

local Solver = require("solver")
local Builtin = require("builtin")

local function lineStr(t)
    local p = {}
    for i = 1, #t do p[i] = tostring(t[i]) end
    return table.concat(p, "")
end

do
    local l = { 0, 0, 0, 0, 0 }
    local ch = Solver.solveLine(l, { 5 }, 5)
    ok("solveLine_full", ch and lineStr(l) == "11111", lineStr(l))
end
do
    local l = { 0, 0, 0, 0, 0 }
    Solver.solveLine(l, { 3 }, 5)
    ok("solveLine_overlap", l[3] == 1, lineStr(l))
end
do
    -- The left-gap case: {1,1,1} in 5 cells is fully determined, and cells 2 and
    -- 4 must come out BLOCKED. Missing the left-gap constraint left them EMPTY.
    local l = { 0, 0, 0, 0, 0 }
    Solver.solveLine(l, { 1, 1, 1 }, 5)
    ok("solveLine_left_gap_blocks", lineStr(l) == "12121", lineStr(l))
end
do
    local l = { 0, 0, 0 }
    Solver.solveLine(l, { 0 }, 3)
    ok("solveLine_zero_clue", lineStr(l) == "222", lineStr(l))
end
do
    local l = { 1, 0, 1, 0, 0 }
    local ch, err = Solver.solveLine(l, { 2 }, 5)
    ok("solveLine_contradiction", ch == nil and err == "contradiction",
       tostring(ch) .. "," .. tostring(err))
end

-- The classic 2x2 switch: no clue set can distinguish the two diagonals.
do
    local amb = { { 1, 0 }, { 0, 1 } }
    local rcs, ccs = Clues.derive(amb, 2, 2)
    ok("solver_detects_2x2_switch",
       Solver.check(2, 2, rcs, ccs, 2000) == "multi",
       Solver.check(2, 2, rcs, ccs, 2000))
end
do
    local solid = { { 1, 1 }, { 1, 1 } }
    local rcs, ccs = Clues.derive(solid, 2, 2)
    ok("solver_solid_is_unique",
       Solver.check(2, 2, rcs, ccs, 2000) == "unique-line",
       Solver.check(2, 2, rcs, ccs, 2000))
end

-- ── every shipped puzzle must have exactly one solution ──────────────────────
--
-- This is the check that matters most in this file. Three of the first six
-- puzzles written for builtin.lua were ambiguous — symmetric line-art usually
-- is, and it is invisible by eye. Without this test, players would hit a puzzle
-- they solved "correctly" that the game refused to accept.
local all = Builtin.list()
local bySize, ambiguous, inconsistent, dupes = {}, {}, {}, {}
local seenGrid, seenId = {}, {}

for _, p in ipairs(all) do
    bySize[p.w] = (bySize[p.w] or 0) + 1

    local verdict = Solver.check(p.w, p.h, p.rowClues, p.colClues, 40000)
    if verdict ~= "unique-line" and verdict ~= "guess" then
        ambiguous[#ambiguous + 1] = ("%s (%dx%d) -> %s"):format(p.name, p.w, p.h, verdict)
    end

    -- Deriving clues from the stored solution must reproduce the stored clues.
    local rcs, ccs = Clues.derive(p.solution, p.w, p.h)
    local match = true
    for r = 1, p.h do if not Clues.equal(rcs[r], p.rowClues[r]) then match = false end end
    for c = 1, p.w do if not Clues.equal(ccs[c], p.colClues[c]) then match = false end end
    if not match then inconsistent[#inconsistent + 1] = p.name end

    -- Two puzzles with the same grid, or the same id, would confuse the records
    -- file (which keys on id) and waste a menu slot.
    local key = {}
    for r = 1, p.h do
        local row = {}
        for c = 1, p.w do row[c] = p.solution[r][c] end
        key[r] = table.concat(row)
    end
    key = table.concat(key, "/")
    if seenGrid[key] then dupes[#dupes + 1] = p.name .. " == " .. seenGrid[key] end
    seenGrid[key] = p.name
    if seenId[p.id] then dupes[#dupes + 1] = "id clash: " .. p.id end
    seenId[p.id] = true
end

print()
print(("bundled puzzles: %d total"):format(#all))
local szs = {}
for w in pairs(bySize) do szs[#szs + 1] = w end
table.sort(szs)
for _, w in ipairs(szs) do
    print(("  %2dx%-2d  %3d"):format(w, w, bySize[w]))
end

ok("all_bundled_puzzles_unique", #ambiguous == 0,
   #ambiguous > 0 and table.concat(ambiguous, "; ") or nil)
ok("all_bundled_clues_consistent", #inconsistent == 0,
   #inconsistent > 0 and table.concat(inconsistent, "; ") or nil)
ok("no_duplicate_puzzles", #dupes == 0,
   #dupes > 0 and table.concat(dupes, "; ") or nil)
ok("bundled_count_reasonable", #all >= 100, "#all=" .. #all)

-- Every size the menu filter offers must actually have puzzles behind it.
for _, w in ipairs(Builtin.sizes()) do
    ok("size_filter_nonempty_" .. w, (bySize[w] or 0) > 0)
end

-- Curated art must sort before generated grids within a size, so the first
-- thing a player sees is recognisable.
do
    local firstOfSize, badOrder = {}, nil
    local lastW, seenGenerated = nil, false
    for _, p in ipairs(all) do
        if p.w ~= lastW then lastW, seenGenerated = p.w, false end
        if not p.curated then seenGenerated = true
        elseif seenGenerated then badOrder = p.name .. " after generated" end
    end
    ok("curated_sorts_first", badOrder == nil, badOrder)
end

-- Share codes must round-trip for every bundled puzzle, since that is how the
-- library and the API represent them.
do
    local bad = nil
    for _, p in ipairs(all) do
        local code = Share.encode({ w = p.w, h = p.h, solution = p.solution })
        local back = code and Share.decode(code)
        if not back then bad = p.name .. ": encode/decode failed"; break end
        for r = 1, p.h do
            for c = 1, p.w do
                if back.solution[r][c] ~= p.solution[r][c] then
                    bad = p.name .. ": grid mismatch"; break
                end
            end
            if bad then break end
        end
        if bad then break end
    end
    ok("all_bundled_sharecode_roundtrip", bad == nil, bad)
end
print()

-- ── summary ──────────────────────────────────────────────────────────────────

print(("host tests: %d passed, %d failed"):format(pass, fail))
os.exit(fail == 0 and 0 or 1)
