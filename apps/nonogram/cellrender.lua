-- cellrender.lua — draws the three visible cell states behind one interface.
--
-- Two backends:
--
--   sprite     a spritesheet whose frame size exactly equals the cell size
--   primitive  drawn from display primitives at any size
--
-- Primitives are the default rather than the fallback, because cell size is
-- computed from the puzzle (32, 24, 14 or 12 px) and img:drawScaledNN only
-- scales UP by whole integers — there is no way to fit a 32px sprite into a
-- 14px cell without it looking mangled. Sprite sheets are therefore an upgrade
-- for the sizes where art actually reads, and the primitive path guarantees
-- every size is correct and legible.

local T = require("theme")
local Board = require("board")

local R = {}
R.__index = R

-- new(gfx, disp, spriteDir) — gfx/disp may be nil on a host test run, in which
-- case only the pure geometry helpers are usable.
function R.new(gfx, disp, spriteDir)
    local self = setmetatable({}, R)
    self.gfx, self.disp = gfx, disp
    self.sheets = {}          -- cell size -> spritesheet

    if gfx and spriteDir then
        -- Sheets are named by the cell size they were drawn for, so an exact
        -- match is the only thing ever used.
        for _, size in ipairs({ 32, 24 }) do
            local path = spriteDir .. "cells_" .. size .. ".png"
            local ok, img = pcall(gfx.image.load, path)
            if ok and img then
                img:setTransparentColor(T.SPRITE_KEY)
                local ok2, sheet = pcall(gfx.spritesheet.newGrid, img, 3, 1, size, size)
                if ok2 and sheet then self.sheets[size] = sheet end
            end
        end
    end

    return self
end

function R:hasSpritesFor(cell)
    return self.sheets[cell] ~= nil
end

-- Frame indices in cells_<size>.png (0-based, as drawFrame expects).
local FRAME_FILLED, FRAME_BLOCKED, FRAME_MAYBE = 0, 1, 2

-- Draws one cell's state. FILLED is handled by graphics.drawPlayfield in a
-- single batched C call, so it is skipped here unless a sprite sheet is in use.
--
-- blink: current blinker state, used to flicker the MAYBE glyph.
function R:drawState(state, x, y, cell, blink)
    if state == Board.EMPTY then return end

    local sheet = self.sheets[cell]
    if sheet then
        if state == Board.FILLED then
            sheet:drawFrame(FRAME_FILLED, x, y)
        elseif state == Board.BLOCKED then
            sheet:drawFrame(FRAME_BLOCKED, x, y)
        elseif state == Board.MAYBE and blink ~= false then
            sheet:drawFrame(FRAME_MAYBE, x, y)
        end
        return
    end

    local disp = self.disp
    if state == Board.BLOCKED then
        -- A dim etched X, inset so it never touches the grid line.
        local i = (cell >= 20) and 4 or 3
        disp.drawLine(x + i, y + i, x + cell - 1 - i, y + cell - 1 - i, T.BLOCK)
        disp.drawLine(x + cell - 1 - i, y + i, x + i, y + cell - 1 - i, T.BLOCK)

    elseif state == Board.MAYBE then
        if blink == false then return end
        -- The 6x8 '?' glyph stays legible down to a 12px cell, which is the
        -- smallest cell size layout.lua will ever produce.
        local gx = x + (cell - T.CLUE_CHAR_W) // 2
        local gy = y + (cell - T.CLUE_CHAR_H) // 2
        disp.drawText(gx, gy, "?", T.MAYBE, false)
    end
end

-- Draws the lit edge on filled cells. Separate from drawPlayfield's flat fill so
-- the two-tone bloom does not need a second full pass over the grid.
function R:drawFilledEdge(x, y, cell)
    local disp = self.disp
    disp.fillHLine(y + 1, x + 1, x + cell - 2, T.FILL_EDGE)
    disp.fillVLine(x + 1, y + 1, y + cell - 2, T.FILL_EDGE)
end

return R
