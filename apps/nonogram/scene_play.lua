-- scene_play.lua — solving a puzzle.
--
-- Input map:
--   arrows        move cursor (auto-repeat when held)
--   ENTER         toggle FILLED
--   space         toggle BLOCKED   (there is no BTN_SPACE; read via getChar)
--   BACKSPACE     toggle MAYBE
--   F2 / F3       undo / redo
--   F4            toggle auto-X assist
--   TAB           paint mode: movement applies the last tool as one undo group
--   ESC           back to the menu

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local gfx   = pc.graphics
local sys   = pc.sys

local T     = require("theme")
local Board = require("board")
local Layout = require("layout")
local Render = require("render")
local CellRender = require("cellrender")
local Records = require("records")

local S = {}

local board, L, cells, blinker
local cx, cy = 1, 1
local playfield, dirty
local startMs, solvedMs = 0, nil
local puzzle
local wasSolved, newBest, firstSolve = false, false, false

-- Active drag-paint stroke: { tool = FILLED|BLOCKED|MAYBE, value = what to write }
local stroke = nil
-- TAB latch: behaves as a stroke that survives key release, for one-handed play.
local sticky = false
-- Space has no button constant, so its held state cannot be read from the button
-- mask like ENTER's can — it is only observable as getChar() repeating while the
-- key is down. This is the grace window after the last ' ' during which space is
-- still treated as held. Must exceed the firmware's key-hold repeat interval or
-- a drag will stutter; must stay short or the stroke over-runs the release.
local SPACE_HOLD_GRACE_MS = 260
local lastSpaceMs = 0

-- Hand-rolled repeat, used only when input.getButtonsRepeated is unavailable.
local REPEAT_DELAY, REPEAT_RATE = 200, 70
local lastDpad, dpadMs = 0, 0

local DPAD = input.BTN_UP | input.BTN_DOWN | input.BTN_LEFT | input.BTN_RIGHT

function S.enter(params)
    puzzle = params and params.puzzle
    assert(puzzle, "scene_play requires a puzzle")

    board = Board.new(puzzle)
    board.autoX = params.autoX or false

    L = Layout.compute(puzzle.w, puzzle.h, puzzle.rowClues, puzzle.colClues)
    cells = CellRender.new(gfx, disp, APP_DIR .. "/sprites/")
    blinker = gfx.animation.blinker.new(420, 220, true)

    cx, cy = 1, 1
    dirty = true
    stroke, sticky, lastSpaceMs = nil, false, 0
    startMs, solvedMs = sys.getTimeMs(), nil
    newBest, firstSolve = false, false

    -- Previous best is shown in the footer while solving, so the player has
    -- something to beat.
    wasSolved = Records.isSolved(puzzle.id)
    Records.markPlayed(puzzle.id)

    sys.log(("NG:PLAY %s %dx%d cell=%d viewport=%dx%d%s")
        :format(puzzle.name, puzzle.w, puzzle.h, L.cell, L.vcols, L.vrows,
                L.scrolls and " scrolling" or ""))
end

function S.exit()
    stroke, sticky = nil, false
    board, L, cells, blinker, playfield = nil, nil, nil, nil, nil
end

-- ── Input ────────────────────────────────────────────────────────────────────

local function moveCursor(dx, dy)
    local nx = cx + dx
    local ny = cy + dy
    if nx < 1 then nx = 1 elseif nx > board.w then nx = board.w end
    if ny < 1 then ny = 1 elseif ny > board.h then ny = board.h end
    if nx == cx and ny == cy then return end
    cx, cy = nx, ny
    if L:ensureVisible(cx, cy) then dirty = true end

    -- Drag-paint: an active stroke writes its value into every cell the cursor
    -- enters. `value` is fixed when the stroke starts (see strokeBegin) so a
    -- drag across mixed cells does not flip-flop.
    if stroke then
        if board:set(cx, cy, stroke.value) then dirty = true end
    end
end

-- ── Drag-painting ────────────────────────────────────────────────────────────
--
-- Holding an action key and moving the cursor sets a run of cells in one go,
-- which is the difference between 9 keypresses and 2 for a long row.
--
-- The value is decided ONCE, from the cell the stroke starts on, using the same
-- toggle rule a single tap uses: starting on an empty cell fills, starting on an
-- already-set cell erases. Re-evaluating per cell would make a drag across a
-- half-filled row alternate fill/erase and be useless.
--
-- Recorded as a single undo group, so one F2 undoes the whole stroke.

local function strokeBegin(tool)
    local curr = board:get(cx, cy)
    local value = (curr == tool) and Board.EMPTY or tool

    board:beginGroup()
    stroke = { tool = tool, value = value }
    if board:set(cx, cy, value) then dirty = true end
end

local function strokeEnd()
    if not stroke then return end
    stroke = nil
    board:endGroup()
end

-- Returns a mask of direction edges for this frame, from the SDK's auto-repeat
-- where available and a hand-rolled timer otherwise.
local function dpadEdges()
    if NG_CAP.repeatIn then
        return input.getButtonsRepeated() & DPAD
    end

    local held = input.getButtons() & DPAD
    local pressed = input.getButtonsPressed() & DPAD
    local now = sys.getTimeMs()
    local out = pressed

    if held ~= 0 then
        if held ~= lastDpad then
            dpadMs = now
        elseif now - dpadMs >= REPEAT_DELAY then
            out = out | held
            dpadMs = now - REPEAT_DELAY + REPEAT_RATE
        end
    end
    lastDpad = held
    return out
end

function S.update(dt)
    local pressed = input.getButtonsPressed()
    local ch = input.getChar()

    -- System-menu requests arrive as flags because those callbacks fire from the
    -- opcode hook, mid-frame.
    local pending = NG_PENDING
    if pending.restart then
        pending.restart = nil
        board = Board.new(puzzle)
        board.autoX = pending.autoX or board.autoX
        startMs, solvedMs = sys.getTimeMs(), nil
        dirty = true
    end
    if pending.toggleAutoX then
        pending.toggleAutoX = nil
        board.autoX = not board.autoX
        pc.ui.toast("Auto-X " .. (board.autoX and "on" or "off"))
    end

    if pressed & input.BTN_ESC ~= 0 then
        NG_SCENES.switch("menu")
        return
    end

    if solvedMs then
        -- Solved: any key returns to the menu.
        if pressed ~= 0 or ch then NG_SCENES.switch("menu") end
        return
    end

    local edges = dpadEdges()
    if edges & input.BTN_UP    ~= 0 then moveCursor(0, -1) end
    if edges & input.BTN_DOWN  ~= 0 then moveCursor(0,  1) end
    if edges & input.BTN_LEFT  ~= 0 then moveCursor(-1, 0) end
    if edges & input.BTN_RIGHT ~= 0 then moveCursor( 1, 0) end

    -- ── Stroke start / continue / end ────────────────────────────────────────
    --
    -- A tap is just a stroke of length one, so taps and drags share one path.

    local held = input.getButtons()
    local enterHeld = (held & input.BTN_ENTER) ~= 0
    local shiftHeld = (held & input.BTN_SHIFT) ~= 0
    local now = sys.getTimeMs()

    if ch == " " then lastSpaceMs = now end
    local spaceHeld = (now - lastSpaceMs) <= SPACE_HOLD_GRACE_MS

    if pressed & input.BTN_ENTER ~= 0 then
        -- SHIFT+ENTER strokes BLOCKED. Unlike space this is fully observable
        -- from the button mask, so it is the reliable way to drag-block.
        strokeEnd()
        strokeBegin(shiftHeld and Board.BLOCKED or Board.FILLED)
        sticky = false
        sys.log(("NG:FILL c=%d r=%d v=%d"):format(cx, cy, board:get(cx, cy)))
    elseif ch == " " and not stroke then
        strokeEnd()
        strokeBegin(Board.BLOCKED)
        sticky = false
    elseif pressed & input.BTN_BACKSPACE ~= 0 then
        strokeEnd()
        strokeBegin(Board.MAYBE)
        sticky = false
    end

    -- End the stroke once its key is no longer down. TAB-latched strokes ignore
    -- release and persist until TAB is pressed again.
    if stroke and not sticky then
        local stillDown
        if stroke.tool == Board.BLOCKED then
            -- Either source could have started a BLOCKED stroke.
            stillDown = spaceHeld or (enterHeld and shiftHeld)
        elseif stroke.tool == Board.FILLED then
            stillDown = enterHeld
        else
            stillDown = (held & input.BTN_BACKSPACE) ~= 0
        end
        if not stillDown then strokeEnd() end
    end

    if pressed & input.BTN_F2 ~= 0 then
        if board:undo() then dirty = true end
    end
    if pressed & input.BTN_F3 ~= 0 then
        if board:redo() then dirty = true end
    end
    if pressed & input.BTN_F4 ~= 0 then
        board.autoX = not board.autoX
        pc.ui.toast("Auto-X " .. (board.autoX and "on" or "off"))
    end

    -- TAB latches a stroke on, for filling long runs without holding a key.
    if pressed & input.BTN_TAB ~= 0 then
        if sticky then
            sticky = false
            strokeEnd()
        else
            strokeEnd()
            strokeBegin(shiftHeld and Board.BLOCKED or Board.FILLED)
            sticky = true
        end
    end

    if board:isSolved() then
        solvedMs = sys.getTimeMs()
        -- Close any in-flight stroke so the winning move is committed as one
        -- undo group and no stroke survives into the solved screen.
        sticky = false
        strokeEnd()
        dirty = true
        local secs = (solvedMs - startMs) // 1000

        newBest, firstSolve = Records.markSolved(puzzle.id, secs, board.moves)

        sys.log(("NG:SOLVED %s moves=%d secs=%d first=%s best=%s")
            :format(puzzle.name, board.moves, secs,
                    tostring(firstSolve), tostring(newBest)))
        pcall(pc.audio.playTone, 880, 90)
        if newBest or firstSolve then
            pcall(pc.audio.playTone, 1320, 110)
        end
    end
end

-- ── Draw ─────────────────────────────────────────────────────────────────────

function S.draw()
    if dirty or not playfield then
        playfield = Render.buildPlayfield(board, L)
        dirty = false
    end

    local blink = blinker:update()

    -- Stage 1: board art (gets scanlined).
    Render.backdrop()
    Render.boardArt(board, L, playfield, cells, blink)

    -- Whole-frame effect. Everything after this point stays crisp.
    Render.scanlines()

    -- Stage 2: text and chrome.
    Render.clues(board, L)
    if not solvedMs then Render.cursor(L, cx, cy, blink) end
    Render.scrollbars(L)

    local filled, need = board:counts()
    Render.header(puzzle.name, ("%d/%d"):format(filled, need))

    if solvedMs then
        local secs = (solvedMs - startMs) // 1000
        local msg = ("ICE BROKEN  %s  %d moves")
            :format(Records.formatTime(secs), board.moves)
        local sub
        if firstSolve then sub = "FIRST CLEAR"
        elseif newBest then sub = "NEW BEST"
        else sub = "BEST " .. Records.formatTime(Records.bestSecs(puzzle.id)) end

        disp.setFont(T.FONT_6X8)
        local w = math.max(disp.textWidth(msg), disp.textWidth(sub))
        local bx = (320 - w - 16) // 2
        Render.panel(bx, 136, w + 16, 38)
        disp.drawText(bx + 8, 143, msg, T.FILL, T.BG_PANEL)
        disp.drawText(bx + 8, 157, sub,
                      (firstSolve or newBest) and T.MAYBE or T.TEXT_DIM,
                      T.BG_PANEL)
        Render.footer("any key: menu", "SOLVED")
    else
        -- An active stroke takes over the status slot: holding a key to paint is
        -- the one mode where the player benefits from live feedback.
        local right
        if stroke then
            local what = (stroke.value == Board.EMPTY) and "ERASE"
                or (stroke.tool == Board.FILLED and "FILL"
                    or stroke.tool == Board.BLOCKED and "BLOCK" or "MARK")
            right = (sticky and "LATCHED " or "PAINT ") .. what
        elseif wasSolved then
            right = "BEST " .. Records.formatTime(Records.bestSecs(puzzle.id))
        else
            right = board.autoX and "F4 auto-X ON" or "F4 auto-X off"
        end
        Render.footer("hold ENTER/SPACE to drag  TAB latch",
                      right)
    end
end

return S
