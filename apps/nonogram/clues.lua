-- clues.lua — pure clue maths. No drawing, no picocalc.* dependency.
--
-- Owns two things: deriving clue lists from a solution grid, and classifying a
-- player's line against its clues to drive the on-screen assists.

local C = {}

C.EMPTY   = 0
C.FILLED  = 1
C.BLOCKED = 2
C.MAYBE   = 3

-- Run-length clue list for one line. `get(i)` returns a cell value for i=1..n.
-- Only FILLED counts as filled; BLOCKED and MAYBE are both "not filled".
--
-- An all-empty line yields {0}, not {} — nonogram convention writes a single
-- zero so the clue gutter shows something, and it keeps sum()/count logic from
-- having to special-case an empty list.
function C.fromLine(get, n)
    local out, run = {}, 0
    for i = 1, n do
        if get(i) == C.FILLED then
            run = run + 1
        elseif run > 0 then
            out[#out + 1] = run
            run = 0
        end
    end
    if run > 0 then out[#out + 1] = run end
    if #out == 0 then out[1] = 0 end
    return out
end

-- Derives rowClues, colClues from a solution grid[r][c] of 0/1.
function C.derive(grid, w, h)
    local rowClues, colClues = {}, {}
    for r = 1, h do
        rowClues[r] = C.fromLine(function(i) return grid[r][i] end, w)
    end
    for c = 1, w do
        colClues[c] = C.fromLine(function(i) return grid[i][c] end, h)
    end
    return rowClues, colClues
end

function C.sum(list)
    local s = 0
    for i = 1, #list do s = s + list[i] end
    return s
end

-- True when two clue lists are identical.
function C.equal(a, b)
    if #a ~= #b then return false end
    for i = 1, #a do if a[i] ~= b[i] then return false end end
    return true
end

-- Classifies a player line against its clue list. Drives both the gutter
-- backlight and the clue-number colour.
--
--   "done"    the filled pattern exactly matches the clues -> retire the clues
--   "over"    more filled cells than the clues allow, or a run too long
--   "neutral" anything else, including a correct count in the wrong arrangement
--
-- There is deliberately no "right count, wrong pattern" state: signalling that
-- would tell the player their count is right without revealing where, which
-- reads as a hint rather than feedback. "over" only ever reports a provable
-- mistake.
function C.classify(get, n, clueList)
    local filled = 0
    for i = 1, n do
        if get(i) == C.FILLED then filled = filled + 1 end
    end

    local want = C.sum(clueList)
    if filled > want then return "over" end

    local actual = C.fromLine(get, n)

    -- A run longer than the largest clue is a mistake even when the total count
    -- is still under budget.
    local maxClue = 0
    for i = 1, #clueList do
        if clueList[i] > maxClue then maxClue = clueList[i] end
    end
    for i = 1, #actual do
        if actual[i] > maxClue then return "over" end
    end

    if C.equal(actual, clueList) then
        -- Matching runs are not sufficient on their own: the line must also be
        -- fully decided, otherwise a partially-drawn line whose runs happen to
        -- match so far would retire its clues early.
        if filled == want then return "done" end
    end

    return "neutral"
end

-- Maximum number of clues a line of length n can hold: alternating filled and
-- gap cells, i.e. ceil(n/2). Used to size the clue gutters.
function C.maxClueCount(n)
    return (n + 1) // 2
end

-- Rendered pixel width of a clue list in the 6x8 clue font: each number costs
-- its digit count plus one separating space.
function C.renderedWidth(list, charW)
    local w = 0
    for i = 1, #list do
        w = w + (#tostring(list[i]) + 1) * charW
    end
    return w
end

return C
