-- render.lua — all board and chrome drawing. Owns the frame's draw ORDER.
--
-- The ordering is the load-bearing part of the cyberpunk look, because
-- applyEffect operates on the WHOLE framebuffer — there is no region-limited
-- variant. So the frame is built in two stages:
--
--    board art  ->  applyEffect("scanline")  ->  crisp chrome and text
--
-- Everything drawn before the effect gets CRT texture; everything after stays
-- sharp. Text goes last and uses the transparent-background mode, so clue
-- numbers sit over the scanlined gutter washes without punching flat boxes
-- through them. Before transparent text existed, this needed the washes drawn
-- AFTER the effect (flat) purely so drawText's opaque bg had something to match.

local T = require("theme")
local Board = require("board")

local R = {}

local disp, gfx, ui

function R.init(picocalc)
    disp, gfx, ui = picocalc.display, picocalc.graphics, picocalc.ui

    -- theme.lua carries the font ids as plain numbers so it stays host-loadable.
    -- Assert they still match the real constants rather than trusting them.
    assert(T.FONT_6X8 == disp.FONT_6X8, "theme FONT_6X8 out of sync")
    assert(T.FONT_SCI_BOLD == disp.FONT_SCIENTIFICA_BOLD,
           "theme FONT_SCI_BOLD out of sync")
end

-- ── Chrome ───────────────────────────────────────────────────────────────────

-- Stepped vertical gradient. 20 fillRects rather than 320 fillVLineGradient
-- calls: after scanlines the banding is invisible and it is 16x fewer C calls.
function R.backdrop()
    disp.clear(T.BG)
    local bands = 20
    local bandH = 320 // bands
    for i = 0, bands - 1 do
        local t = i / (bands - 1)
        local g = math.floor(6 + t * 14)
        disp.fillRect(0, i * bandH, 320, bandH, T.rgb(0, g, math.floor(g * 0.4)))
    end
end

function R.header(title, right)
    disp.fillRect(0, 0, 320, T.HEADER_H, T.BG_PANEL)
    disp.fillHLine(T.HEADER_H - 1, 0, 319, T.GRID_MAJOR)
    disp.setFont(T.HEADER_FONT)
    disp.drawText(T.MARGIN_L, 3, title, T.FILL, false)
    if right then
        disp.setFont(T.FONT_6X8)
        local w = disp.textWidth(right)
        disp.drawText(320 - T.MARGIN_R - w, 5, right, T.TEXT_DIM, false)
    end
    disp.setFont(T.FONT_6X8)
end

function R.footer(left, right)
    local y = 320 - T.FOOTER_H
    disp.fillRect(0, y, 320, T.FOOTER_H, T.BG_PANEL)
    disp.fillHLine(y, 0, 319, T.GRID_MAJOR)
    disp.setFont(T.FONT_6X8)
    if left then disp.drawText(T.MARGIN_L, y + 6, left, T.TEXT_DIM, false) end
    if right then
        local w = disp.textWidth(right)
        disp.drawText(320 - T.MARGIN_R - w, y + 6, right, T.CLUE, false)
    end
end

-- A bordered panel with a 2-step outer glow. No alpha needed — concentric
-- rectangles at decreasing brightness read as a neon halo.
function R.panel(x, y, w, h, title)
    gfx.fillBorderedRect(x, y, w, h, T.BG_PANEL, T.GRID_MAJOR)
    disp.drawRect(x - 1, y - 1, w + 2, h + 2, T.GRID)
    if title then
        disp.setFont(T.FONT_6X8)
        disp.drawText(x + 4, y + 3, title, T.CLUE, T.BG_PANEL)
    end
end

-- ── Board ────────────────────────────────────────────────────────────────────

-- Colour for a line's gutter wash, or nil when the line needs no highlight.
local function laneColor(state)
    if state == "done" then return T.LANE_DONE end
    if state == "over" then return T.LANE_OVER end
    return nil
end

-- Builds the playfield table graphics.drawPlayfield consumes: grid lines plus
-- every filled cell in ONE C call. Rebuilt only when the board changes, not per
-- frame — see scene_play's dirty flag.
function R.buildPlayfield(board, L)
    local pf = {}
    for vr = 1, L.vrows do
        local r = vr + L.oy
        local row = {}
        local srow = board.cells[r]
        for vc = 1, L.vcols do
            local c = vc + L.ox
            if srow[c] == Board.FILLED then row[vc] = T.FILL end
        end
        pf[vr] = row
    end
    return pf
end

-- Stage 1: everything that should receive the scanline texture.
function R.boardArt(board, L, playfield, cells, blink)
    -- Gutter washes go here, BEFORE the effect, so they are textured like the
    -- rest of the board rather than sitting on top as flat colour.
    for vr = 1, L.vrows do
        local r = vr + L.oy
        local col = laneColor(board.rowState[r])
        if col then
            disp.fillRect(L.gutterX, L.gridY + (vr - 1) * L.cell,
                          L.gutterW, L.cell, col)
        end
    end
    for vc = 1, L.vcols do
        local c = vc + L.ox
        local col = laneColor(board.colState[c])
        if col then
            disp.fillRect(L.gridX + (vc - 1) * L.cell, L.gutterY,
                          L.cell, L.gutterH, col)
        end
    end

    -- Grid lines + all filled cells in a single C call.
    gfx.drawPlayfield(playfield, L.gridX, L.gridY, L.cell,
                      L.vcols, L.vrows, T.GRID)

    -- Every 5th line brighter, so runs can be counted by eye.
    for vc = 0, L.vcols do
        local c = vc + L.ox
        if c % 5 == 0 then
            disp.fillVLine(L.gridX + vc * L.cell, L.gridY, L.gridBottom - 1,
                           T.GRID_MAJOR)
        end
    end
    for vr = 0, L.vrows do
        local r = vr + L.oy
        if r % 5 == 0 then
            disp.fillHLine(L.gridY + vr * L.cell, L.gridX, L.gridRight - 1,
                           T.GRID_MAJOR)
        end
    end

    -- Non-empty, non-filled cells: BLOCKED and MAYBE glyphs. Only these need a
    -- per-cell call; FILLED came from drawPlayfield above.
    for vr = 1, L.vrows do
        local r = vr + L.oy
        local srow = board.cells[r]
        for vc = 1, L.vcols do
            local c = vc + L.ox
            local v = srow[c]
            if v == Board.BLOCKED or v == Board.MAYBE then
                cells:drawState(v, L.gridX + (vc - 1) * L.cell,
                                L.gridY + (vr - 1) * L.cell, L.cell, blink)
            end
        end
    end
end

-- Stage 2: clue text, drawn AFTER the effect so it stays crisp. Uses
-- transparent-background text so the scanlined lane wash shows through.
function R.clues(board, L)
    disp.setFont(T.CLUE_FONT)

    for vr = 1, L.vrows do
        local r = vr + L.oy
        local list = board.rowClues[r]
        local done = (board.rowState[r] == "done")
        local col = done and T.CLUE_DONE or T.CLUE

        -- Right-aligned against the grid edge, so the list grows leftwards.
        local total = 0
        for i = 1, #list do total = total + (#tostring(list[i]) + 1) * T.CLUE_CHAR_W end
        local x, y = L:rowClueOrigin(r, total)
        if x then
            for i = 1, #list do
                local s = tostring(list[i])
                disp.drawText(x, y, s, col, false)
                x = x + (#s + 1) * T.CLUE_CHAR_W
            end
        end
    end

    for vc = 1, L.vcols do
        local c = vc + L.ox
        local list = board.colClues[c]
        local done = (board.colState[c] == "done")
        local col = done and T.CLUE_DONE or T.CLUE
        local k = #list
        for j = 1, k do
            local s = tostring(list[j])
            local x, y = L:colClueSlot(c, j, k, #s)
            if x then disp.drawText(x, y, s, col, false) end
        end
    end
end

-- Cursor: a bright ring plus a full-width/height crosshair tint on the gutters,
-- so the player can see which row and column they are on without counting.
function R.cursor(L, cx, cy, blink)
    local x, y, cell = L:cellRect(cx, cy)
    if not x then return end

    -- Crosshair ticks in the gutters.
    disp.fillRect(L.gutterX, y, L.gutterW, 1, T.CURSOR)
    disp.fillRect(x, L.gutterY, 1, L.gutterH, T.CURSOR)

    disp.drawRect(x, y, cell, cell, T.CURSOR)
    if blink then
        disp.drawRect(x + 1, y + 1, cell - 2, cell - 2, T.CURSOR)
    end
end

-- Scrollbars, drawn only when the viewport is smaller than the board.
function R.scrollbars(L)
    if not L.scrolls then return end

    if L.vcols < L.cols then
        local track = L.gridW
        local w = math.max(6, track * L.vcols // L.cols)
        local x = L.gridX + (track - w) * L.ox // math.max(1, L.cols - L.vcols)
        disp.fillRect(L.gridX, L.gridBottom + 1, track, 2, T.GRID)
        disp.fillRect(x, L.gridBottom + 1, w, 2, T.FILL)
    end

    if L.vrows < L.rows then
        local track = L.gridH
        local h = math.max(6, track * L.vrows // L.rows)
        local y = L.gridY + (track - h) * L.oy // math.max(1, L.rows - L.vrows)
        disp.fillRect(L.gridRight + 1, L.gridY, 2, track, T.GRID)
        disp.fillRect(L.gridRight + 1, y, 2, h, T.FILL)
    end
end

function R.scanlines()
    disp.applyEffect("scanline", T.SCANLINE_INTENSITY)
end

-- Completion tick, drawn from two lines rather than a glyph: the bitmap fonts
-- only cover ASCII 0x20-0x7E, so there is no checkmark character available.
-- `size` is the box the tick fits inside (8-12px reads well at list scale).
function R.tick(x, y, size, color)
    local s = size or 8
    -- Short down-stroke then a longer up-stroke, thickened by drawing each
    -- stroke twice one pixel apart — a 1px tick disappears under scanlines.
    local x0, y0 = x, y + s // 2
    local x1, y1 = x + s // 3, y + s - 1
    local x2, y2 = x + s - 1, y
    for d = 0, 1 do
        disp.drawLine(x0, y0 + d, x1, y1 + d, color)
        disp.drawLine(x1, y1 + d, x2, y2 + d, color)
    end
end

return R
