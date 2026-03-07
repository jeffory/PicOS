-- Burn-In Fixer for PicOS
-- Cycles through colors and patterns to exercise LCD pixels.
-- Run for several minutes to help loosen stuck or burned-in pixels.

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local sys   = pc.sys

local W = disp.getWidth()    -- 320
local H = disp.getHeight()   -- 320
local STATUS_H = 18          -- height of the bottom status bar
local DRAW_H   = H - STATUS_H

-- ── Palette ───────────────────────────────────────────────────────────────────

local BLACK   = disp.BLACK
local WHITE   = disp.WHITE
local RED     = disp.RED
local GREEN   = disp.GREEN
local BLUE    = disp.BLUE
local CYAN    = disp.CYAN
local MAGENTA = disp.rgb(255, 0, 255)
local YELLOW  = disp.YELLOW
local GRAY    = disp.GRAY

-- ── Helpers ───────────────────────────────────────────────────────────────────

local function fmt_time(ms)
    local s = math.floor(ms / 1000)
    local m = math.floor(s / 60)
    return string.format("%d:%02d", m, s % 60)
end

-- Draw a thin progress bar + text status at the bottom of the screen.
-- Called after each mode's draw so it always renders on top.
local function draw_status(elapsed_ms, duration_ms, mode_name, detail, total_start_ms)
    local sy = H - STATUS_H
    disp.fillRect(0, sy, W, STATUS_H, BLACK)

    -- Progress bar (1px tall, cyan fill)
    local bar_w = math.floor(W * math.min(elapsed_ms, duration_ms) / duration_ms)
    disp.fillRect(0, sy, bar_w, 1, CYAN)

    -- Left label: mode name + optional detail
    local left = mode_name
    if detail ~= "" then left = left .. " - " .. detail end
    disp.drawText(2, sy + 4, left, WHITE, BLACK)

    -- Right label: total elapsed + hint
    local right = fmt_time(sys.getTimeMs() - total_start_ms) .. "  Esc:Exit"
    -- 6 px per char (built-in 6x8 font), right-align with 2px margin
    local rx = W - #right * 6 - 2
    disp.drawText(rx, sy + 4, right, GRAY, BLACK)
end

-- ── Mode definitions ──────────────────────────────────────────────────────────
-- Each mode is a table with:
--   name     string
--   duration number  (total ms before auto-advancing)
--   init()           called when mode becomes active
--   draw(elapsed_ms) called every frame
--   detail(elapsed_ms) -> string  shown in status bar

local modes = {}

-- ── Mode 1: Solid color cycle ─────────────────────────────────────────────────
-- Holds each of 8 pure colors for 3 seconds.
-- Exercises individual color channels to capacity.
do
    local COLORS = { BLACK, WHITE, RED, GREEN, BLUE, CYAN, MAGENTA, YELLOW }
    local NAMES  = { "Black","White","Red","Green","Blue","Cyan","Magenta","Yellow" }
    local PER_MS = 3000

    modes[#modes + 1] = {
        name     = "Solid Colors",
        duration = #COLORS * PER_MS,
        init     = function() end,
        draw     = function(e)
            local i = math.floor(e / PER_MS) % #COLORS + 1
            disp.clear(COLORS[i])
        end,
        detail   = function(e)
            local i = math.floor(e / PER_MS) % #COLORS + 1
            return NAMES[i]
        end,
    }
end

-- ── Mode 2: Color gradients ───────────────────────────────────────────────────
-- Sweeps each primary channel (R, G, B) and white from dark to full brightness
-- across the screen width.  Exercises every brightness level per channel.
do
    local CHANNELS = {
        { label = "Red",   fn = function(x) return disp.rgb(x, 0, 0)    end },
        { label = "Green", fn = function(x) return disp.rgb(0, x, 0)    end },
        { label = "Blue",  fn = function(x) return disp.rgb(0, 0, x)    end },
        { label = "White", fn = function(x) return disp.rgb(x, x, x)    end },
    }
    local PER_MS = 4000

    modes[#modes + 1] = {
        name     = "Gradients",
        duration = #CHANNELS * PER_MS,
        init     = function() end,
        draw     = function(e)
            local ci = math.floor(e / PER_MS) % #CHANNELS + 1
            local fn = CHANNELS[ci].fn
            -- Draw 2-pixel-wide vertical strips; 160 C calls, very fast
            for x = 0, W - 1, 2 do
                local v = math.floor(x * 255 / W)
                disp.fillRect(x, 0, 2, DRAW_H, fn(v))
            end
        end,
        detail   = function(e)
            local ci = math.floor(e / PER_MS) % #CHANNELS + 1
            return CHANNELS[ci].label
        end,
    }
end

-- ── Mode 3: Horizontal scan line ──────────────────────────────────────────────
-- A bright white bar scans top-to-bottom against a black field.
-- Targets stuck pixels that only respond to local contrast change.
do
    local scan_y = 0
    local SPEED  = 3   -- pixels per frame
    local THICK  = 5   -- bar thickness in pixels

    modes[#modes + 1] = {
        name     = "Scan Line",
        duration = 12000,
        init     = function() scan_y = 0 end,
        draw     = function(e)
            disp.clear(BLACK)
            disp.fillRect(0, scan_y, W, THICK, WHITE)
            scan_y = scan_y + SPEED
            if scan_y >= DRAW_H then scan_y = 0 end
        end,
        detail   = function(e) return "" end,
    }
end

-- ── Mode 4: Vertical scan line ────────────────────────────────────────────────
-- Same as above but sweeps left-to-right, covering a different axis.
do
    local scan_x = 0
    local SPEED  = 3
    local THICK  = 5

    modes[#modes + 1] = {
        name     = "V-Scan",
        duration = 12000,
        init     = function() scan_x = 0 end,
        draw     = function(e)
            disp.clear(BLACK)
            disp.fillRect(scan_x, 0, THICK, DRAW_H, WHITE)
            scan_x = scan_x + SPEED
            if scan_x >= W then scan_x = 0 end
        end,
        detail   = function(e) return "" end,
    }
end

-- ── Mode 5: Alternating horizontal stripes ────────────────────────────────────
-- Even/odd pixel rows flip between black and white every half-second.
-- Every pixel is forced to both extremes in rapid succession.
do
    local FLIP_MS = 500

    modes[#modes + 1] = {
        name     = "H-Stripes",
        duration = 8000,
        init     = function() end,
        draw     = function(e)
            local phase = math.floor(e / FLIP_MS) % 2
            disp.clear(BLACK)
            for y = phase, DRAW_H - 1, 2 do
                disp.fillRect(0, y, W, 1, WHITE)
            end
        end,
        detail   = function(e) return "" end,
    }
end

-- ── Mode 6: Alternating vertical stripes ──────────────────────────────────────
do
    local FLIP_MS = 500

    modes[#modes + 1] = {
        name     = "V-Stripes",
        duration = 8000,
        init     = function() end,
        draw     = function(e)
            local phase = math.floor(e / FLIP_MS) % 2
            disp.clear(BLACK)
            for x = phase, W - 1, 2 do
                disp.fillRect(x, 0, 1, DRAW_H, WHITE)
            end
        end,
        detail   = function(e) return "" end,
    }
end

-- ── Mode 7: Black/white flash ─────────────────────────────────────────────────
-- Rapidly alternates full-screen black and white at ~5 Hz.
-- High-contrast, high-frequency exercise for stubborn pixels.
do
    local HALF_MS = 100   -- each half of the cycle

    modes[#modes + 1] = {
        name     = "Flash",
        duration = 6000,
        init     = function() end,
        draw     = function(e)
            local phase = math.floor(e / HALF_MS) % 2
            disp.clear(phase == 0 and WHITE or BLACK)
        end,
        detail   = function(e) return "B/W" end,
    }
end

-- ── Mode 8: Color flash ───────────────────────────────────────────────────────
-- Cycles R→G→B→W at ~5 Hz to exercise every sub-pixel combination.
do
    local HALF_MS = 150
    local COLS = { RED, GREEN, BLUE, WHITE }

    modes[#modes + 1] = {
        name     = "Color Flash",
        duration = 8000,
        init     = function() end,
        draw     = function(e)
            local i = math.floor(e / HALF_MS) % #COLS + 1
            disp.clear(COLS[i])
        end,
        detail   = function(e) return "RGB" end,
    }
end

-- ── Mode 9: Random colored blocks ─────────────────────────────────────────────
-- Paints random-sized, random-colored rectangles every frame.
-- Chaotic stimulation for pixels that resist uniform patterns.
do
    local PALETTE = { WHITE, RED, GREEN, BLUE, CYAN, MAGENTA, YELLOW }
    math.randomseed(sys.getTimeMs())

    modes[#modes + 1] = {
        name     = "Random",
        duration = 10000,
        init     = function() end,
        draw     = function(e)
            disp.clear(BLACK)
            for _ = 1, 60 do
                local x = math.random(0, W  - 1)
                local y = math.random(0, DRAW_H - 1)
                local s = math.random(4, 32)
                local c = PALETTE[math.random(#PALETTE)]
                disp.fillRect(x, y, s, s, c)
            end
        end,
        detail   = function(e) return "" end,
    }
end

-- ── Main loop ──────────────────────────────────────────────────────────────────

local cur          = 1
local total_start  = sys.getTimeMs()
local mode_start   = total_start

modes[cur].init()

while true do
    input.update()
    local pressed = input.getButtonsPressed()

    -- Esc: quit
    if pressed & input.BTN_ESC ~= 0 then
        return
    end

    -- Enter: skip to next mode immediately
    if pressed & input.BTN_ENTER ~= 0 then
        cur = cur % #modes + 1
        mode_start = sys.getTimeMs()
        modes[cur].init()
    end

    local now     = sys.getTimeMs()
    local elapsed = now - mode_start

    -- Auto-advance when mode duration expires
    if elapsed >= modes[cur].duration then
        cur = cur % #modes + 1
        mode_start = now
        elapsed    = 0
        modes[cur].init()
    end

    -- Draw effect (fills framebuffer up to DRAW_H)
    modes[cur].draw(elapsed)

    -- Overlay status bar
    draw_status(elapsed, modes[cur].duration, modes[cur].name,
                modes[cur].detail(elapsed), total_start)

    disp.flush()
end
