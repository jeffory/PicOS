-- layout.lua — all board geometry. Pure arithmetic, no drawing, no picocalc.*.
--
-- Screen is 320x320. The job is to fit, top to bottom:
--   header | column-clue gutter | grid | footer
-- and left to right:
--   row-clue gutter | grid
--
-- Two constraints drive everything:
--
--  * MIN_CELL = 12. A two-digit column clue is 12px wide in the 6x8 clue font
--    and is drawn inside its column, so a cell narrower than 12px would bleed
--    clue digits into the neighbouring column. This is what forces 20x20 onto a
--    scrolling viewport: 20 rows of column clues eat 80px, leaving 200px for 20
--    rows = 10px each, under the floor.
--
--  * Gutters are sized from the puzzle's ACTUAL clues, not the worst case. A
--    line of length n can hold at most ceil(n/2) clues, but that only happens
--    when every clue is 1. Measuring the real lists keeps positions stable per
--    puzzle (so clue text never jitters between frames) while giving small-clue
--    puzzles a bigger grid than a fixed-slot layout would.

local T = require("theme")
local Clues = require("clues")

local L = {}
L.__index = L

local SCREEN_W, SCREEN_H = 320, 320
local MIN_CELL = 12
local MAX_CELL = 32

-- compute(cols, rows [, rowClues] [, colClues]) -> layout
--
-- Without clue lists it sizes for the worst case (all clues = 1), which is what
-- the create-mode editor wants before a solution exists: the grid must not
-- resize as the player draws.
function L.compute(cols, rows, rowClues, colClues)
    local self = setmetatable({}, L)

    self.cols, self.rows = cols, rows

    -- ── Gutter sizes ─────────────────────────────────────────────────────────

    local colClueRows
    if colClues then
        colClueRows = 0
        for c = 1, cols do
            local n = #colClues[c]
            if n > colClueRows then colClueRows = n end
        end
    else
        colClueRows = Clues.maxClueCount(rows)
    end
    if colClueRows < 1 then colClueRows = 1 end

    local rowGutterW
    if rowClues then
        rowGutterW = 0
        for r = 1, rows do
            local w = Clues.renderedWidth(rowClues[r], T.CLUE_CHAR_W)
            if w > rowGutterW then rowGutterW = w end
        end
    else
        rowGutterW = Clues.maxClueCount(cols) * 2 * T.CLUE_CHAR_W
    end
    if rowGutterW < 2 * T.CLUE_CHAR_W then rowGutterW = 2 * T.CLUE_CHAR_W end

    self.colClueRows = colClueRows
    self.gutterH = colClueRows * T.CLUE_CHAR_H
    self.gutterW = rowGutterW

    -- ── Cell size ────────────────────────────────────────────────────────────

    local availH = SCREEN_H - T.HEADER_H - T.GAP - self.gutterH - T.FOOTER_H
    local availW = SCREEN_W - T.MARGIN_L - T.MARGIN_R - self.gutterW

    local cell = MAX_CELL
    if availH // rows < cell then cell = availH // rows end
    if availW // cols < cell then cell = availW // cols end
    if cell < MIN_CELL then cell = MIN_CELL end
    self.cell = cell

    -- ── Viewport ─────────────────────────────────────────────────────────────

    local vcols = availW // cell
    local vrows = availH // cell
    if vcols > cols then vcols = cols end
    if vrows > rows then vrows = rows end
    if vcols < 1 then vcols = 1 end
    if vrows < 1 then vrows = 1 end

    self.vcols, self.vrows = vcols, vrows
    self.scrolls = (vcols < cols) or (vrows < rows)
    self.ox, self.oy = 0, 0          -- viewport origin, 0-based cell offsets

    -- ── Absolute positions ───────────────────────────────────────────────────

    self.gridW = vcols * cell
    self.gridH = vrows * cell

    -- Centre the (gutter + grid) block horizontally; top-align vertically so
    -- leftover height becomes a usable panel rather than dead space.
    local blockW = self.gutterW + self.gridW
    local x0 = (SCREEN_W - blockW) // 2
    if x0 < T.MARGIN_L then x0 = T.MARGIN_L end

    self.gutterX = x0
    self.gridX   = x0 + self.gutterW
    self.gutterY = T.HEADER_H + T.GAP
    self.gridY   = self.gutterY + self.gutterH

    self.gridRight  = self.gridX + self.gridW
    self.gridBottom = self.gridY + self.gridH

    -- Leftover vertical space between the grid and the footer, if any.
    local freeTop = self.gridBottom + 2
    local freeH = SCREEN_H - T.FOOTER_H - freeTop
    if freeH >= 12 then
        self.freeRect = { x = T.MARGIN_L, y = freeTop,
                          w = SCREEN_W - T.MARGIN_L - T.MARGIN_R, h = freeH }
    end

    return self
end

-- ── Viewport helpers ─────────────────────────────────────────────────────────

function L:isVisible(c, r)
    return c > self.ox and c <= self.ox + self.vcols
       and r > self.oy and r <= self.oy + self.vrows
end

-- Pixel rect of a cell, or nil when scrolled out of view. Every draw path goes
-- through this, which is why no clipping rectangle is needed.
function L:cellRect(c, r)
    if not self:isVisible(c, r) then return nil end
    return self.gridX + (c - 1 - self.ox) * self.cell,
           self.gridY + (r - 1 - self.oy) * self.cell,
           self.cell
end

-- Scrolls the viewport so (c,r) is visible with a margin, and reports whether
-- anything moved.
function L:ensureVisible(c, r, margin)
    margin = margin or 2
    local ox, oy = self.ox, self.oy

    if self.vcols < self.cols then
        local lo = c - 1 - margin
        local hi = c - self.vcols + margin
        if ox > lo then ox = lo end
        if ox < hi then ox = hi end
        local maxox = self.cols - self.vcols
        if ox < 0 then ox = 0 elseif ox > maxox then ox = maxox end
    end

    if self.vrows < self.rows then
        local lo = r - 1 - margin
        local hi = r - self.vrows + margin
        if oy > lo then oy = lo end
        if oy < hi then oy = hi end
        local maxoy = self.rows - self.vrows
        if oy < 0 then oy = 0 elseif oy > maxoy then oy = maxoy end
    end

    local moved = (ox ~= self.ox) or (oy ~= self.oy)
    self.ox, self.oy = ox, oy
    return moved
end

-- ── Clue text placement ──────────────────────────────────────────────────────

-- Row clues are right-aligned against the grid's left edge, so the list grows
-- leftwards into the gutter.
--
-- Returns x, y for drawing `text`, or nil if the row is scrolled out of view.
function L:rowClueOrigin(r, renderedW)
    if r <= self.oy or r > self.oy + self.vrows then return nil end
    local y = self.gridY + (r - 1 - self.oy) * self.cell
              + (self.cell - T.CLUE_CHAR_H) // 2
    return self.gridX - renderedW, y
end

-- Column clues stack upwards from the grid's top edge, bottom-aligned so the
-- last clue always sits adjacent to the grid.
--
-- j is 1-based within the list; k is the list length.
function L:colClueSlot(c, j, k, digits)
    if c <= self.ox or c > self.ox + self.vcols then return nil end
    local x = self.gridX + (c - 1 - self.ox) * self.cell
              + (self.cell - digits * T.CLUE_CHAR_W) // 2
    local y = self.gridY - (k - j + 1) * T.CLUE_CHAR_H
    return x, y
end

return L
