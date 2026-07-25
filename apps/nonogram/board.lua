-- board.lua — mutable player state: cells, undo, cached line states, win check.
-- Pure logic, no drawing, no picocalc.*.

local Clues = require("clues")

local B = {}
B.__index = B

local EMPTY, FILLED, BLOCKED, MAYBE = 0, 1, 2, 3
B.EMPTY, B.FILLED, B.BLOCKED, B.MAYBE = EMPTY, FILLED, BLOCKED, MAYBE

-- new(puzzle) where puzzle = {w, h, rowClues, colClues}
function B.new(puzzle)
    local self = setmetatable({}, B)
    self.w, self.h = puzzle.w, puzzle.h
    self.rowClues, self.colClues = puzzle.rowClues, puzzle.colClues

    self.cells = {}
    for r = 1, self.h do
        self.cells[r] = {}
        for c = 1, self.w do self.cells[r][c] = EMPTY end
    end

    self.undoStack, self.redoStack = {}, {}
    self.group = nil            -- open transaction, see beginGroup
    self.autoX = false          -- assist: auto-block satisfied lines
    self.moves = 0

    -- Line states are cached because recomputing all 2n lines every frame is
    -- wasteful: an edit can only ever change the one row and one column it
    -- touched.
    self.rowState, self.colState = {}, {}
    self:recomputeAll()

    return self
end

function B:get(c, r)
    local row = self.cells[r]
    return row and row[c] or nil
end

-- ── Line state ───────────────────────────────────────────────────────────────

function B:rowGetter(r)
    local row = self.cells[r]
    return function(i) return row[i] end
end

function B:colGetter(c)
    local cells = self.cells
    return function(i) return cells[i][c] end
end

function B:recomputeRow(r)
    self.rowState[r] = Clues.classify(self:rowGetter(r), self.w, self.rowClues[r])
end

function B:recomputeCol(c)
    self.colState[c] = Clues.classify(self:colGetter(c), self.h, self.colClues[c])
end

function B:recomputeAll()
    for r = 1, self.h do self:recomputeRow(r) end
    for c = 1, self.w do self:recomputeCol(c) end
end

-- ── Mutation ─────────────────────────────────────────────────────────────────

-- beginGroup/endGroup make a drag-paint stroke undo as one action instead of
-- one-cell-at-a-time.
function B:beginGroup()
    if not self.group then self.group = {} end
end

function B:endGroup()
    local g = self.group
    self.group = nil
    if g and #g > 0 then
        self.undoStack[#self.undoStack + 1] = g
        self.redoStack = {}
    end
end

-- Records a change and applies it. Returns true if anything changed.
function B:set(c, r, v)
    if c < 1 or c > self.w or r < 1 or r > self.h then return false end
    local old = self.cells[r][c]
    if old == v then return false end

    local entry = { c = c, r = r, from = old, to = v }

    if self.group then
        self.group[#self.group + 1] = entry
    else
        self.undoStack[#self.undoStack + 1] = { entry }
        self.redoStack = {}
    end

    self.cells[r][c] = v
    self.moves = self.moves + 1
    self:recomputeRow(r)
    self:recomputeCol(c)

    if self.autoX then self:applyAutoX(c, r) end
    return true
end

-- Cycles a cell through a state ring. Two rings so the two action keys each
-- have a natural toggle: FILLED and BLOCKED both return to EMPTY.
function B:toggleFilled(c, r)
    local v = self:get(c, r)
    return self:set(c, r, v == FILLED and EMPTY or FILLED)
end

function B:toggleBlocked(c, r)
    local v = self:get(c, r)
    return self:set(c, r, v == BLOCKED and EMPTY or BLOCKED)
end

function B:toggleMaybe(c, r)
    local v = self:get(c, r)
    return self:set(c, r, v == MAYBE and EMPTY or MAYBE)
end

-- Assist: once a line is satisfied, its remaining EMPTY cells are provably
-- blocked, so fill them in. Runs inside the caller's undo group where one
-- exists, so an auto-X cascade undoes together with the edit that caused it.
function B:applyAutoX(c, r)
    local opened = false
    if not self.group then self:beginGroup(); opened = true end

    if self.rowState[r] == "done" then
        for i = 1, self.w do
            if self.cells[r][i] == EMPTY then
                self.cells[r][i] = BLOCKED
                self.group[#self.group + 1] = { c = i, r = r, from = EMPTY, to = BLOCKED }
                self:recomputeCol(i)
            end
        end
    end

    if self.colState[c] == "done" then
        for i = 1, self.h do
            if self.cells[i][c] == EMPTY then
                self.cells[i][c] = BLOCKED
                self.group[#self.group + 1] = { c = c, r = i, from = EMPTY, to = BLOCKED }
                self:recomputeRow(i)
            end
        end
    end

    if opened then self:endGroup() end
end

-- ── Undo / redo ──────────────────────────────────────────────────────────────

local function applyGroup(self, group, reverse)
    local touchedRows, touchedCols = {}, {}
    for i = 1, #group do
        local e = group[i]
        self.cells[e.r][e.c] = reverse and e.from or e.to
        touchedRows[e.r] = true
        touchedCols[e.c] = true
    end
    for r in pairs(touchedRows) do self:recomputeRow(r) end
    for c in pairs(touchedCols) do self:recomputeCol(c) end
end

function B:undo()
    local g = table.remove(self.undoStack)
    if not g then return false end
    applyGroup(self, g, true)
    self.redoStack[#self.redoStack + 1] = g
    return true
end

function B:redo()
    local g = table.remove(self.redoStack)
    if not g then return false end
    applyGroup(self, g, false)
    self.undoStack[#self.undoStack + 1] = g
    return true
end

-- ── Progress ─────────────────────────────────────────────────────────────────

function B:counts()
    local filled, need = 0, 0
    for r = 1, self.h do
        for c = 1, self.w do
            if self.cells[r][c] == FILLED then filled = filled + 1 end
        end
    end
    for r = 1, self.h do need = need + Clues.sum(self.rowClues[r]) end
    return filled, need
end

-- Solved when every row AND every column is satisfied. Checking both is not
-- redundant: matching all rows guarantees the right total but not the right
-- arrangement within columns.
function B:isSolved()
    for r = 1, self.h do
        if self.rowState[r] ~= "done" then return false end
    end
    for c = 1, self.w do
        if self.colState[c] ~= "done" then return false end
    end
    return true
end

function B:hasErrors()
    for r = 1, self.h do if self.rowState[r] == "over" then return true end end
    for c = 1, self.w do if self.colState[c] == "over" then return true end end
    return false
end

-- ── Serialization ────────────────────────────────────────────────────────────

-- Flat 0-3 cell values, row-major, for the progress file.
function B:snapshot()
    local out = {}
    local n = 0
    for r = 1, self.h do
        for c = 1, self.w do
            n = n + 1
            out[n] = self.cells[r][c]
        end
    end
    return out
end

function B:restore(flat)
    if not flat or #flat ~= self.w * self.h then return false end
    local n = 0
    for r = 1, self.h do
        for c = 1, self.w do
            n = n + 1
            local v = flat[n]
            self.cells[r][c] = (v >= 0 and v <= 3) and v or EMPTY
        end
    end
    self.undoStack, self.redoStack = {}, {}
    self:recomputeAll()
    return true
end

return B
