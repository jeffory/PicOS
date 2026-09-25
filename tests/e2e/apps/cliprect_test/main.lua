-- Clip Rect + Partial Flush Test — fixture for setClipRect/clearClipRect/
-- getClipRect and flushRows/flushRegion.
--
-- The load-bearing cases:
--   * A fillRect that crosses every clip edge must only change pixels inside
--     the clip — if the simulator stub ignores the clip this passes on
--     hardware and silently fails here (the exact class of bug this suite
--     exists to catch).
--   * flushRows must present ONLY the requested rows, and must NOT swap
--     buffers (a following plain flush presents the same draw buffer).
--   * flushRegion presents only its band but DOES swap.
--
-- Lua cannot read pixels back, so this app only *draws* and the harness
-- probes with get_pixel. Each ENTER advances one phase.

local pc    = picocalc
local disp  = pc.display
local input = pc.input

local RED     = disp.rgb(255, 0, 0)
local BLUE    = disp.rgb(0, 0, 255)
local GREEN   = disp.rgb(0, 255, 0)
local BLACK   = disp.rgb(0, 0, 0)
local MAGENTA = disp.rgb(255, 0, 255)

local CX, CY, CW, CH = 64, 64, 32, 32 -- clip rect (x 64..95, y 64..95)
local FX, FY, FW, FH = 48, 48, 64, 64 -- fill rect crossing all four clip edges

pc.sys.log(("CR:RECTS clip=%d,%d,%d,%d fill=%d,%d,%d,%d")
    :format(CX, CY, CW, CH, FX, FY, FW, FH))

local phases = {
    -- Baseline plus getClipRect coverage: values are asserted from the log
    -- because Lua cannot read pixels back.
    { name = "backdrop", draw = function()
        disp.setClipRect(-16, -16, 400, 400) -- must clamp to the framebuffer
        pc.sys.log(("CR:CLIPFULL %d %d %d %d"):format(disp.getClipRect()))
        disp.setClipRect(CX, CY, CW, CH)
        pc.sys.log(("CR:CLIP %d %d %d %d"):format(disp.getClipRect()))
        disp.clearClipRect()
        disp.clear(RED)
        disp.flush()
    end },

    -- Fill crossing the clip boundary: only the clip interior may turn blue.
    -- clear() deliberately ignores the clip, so it goes before setClipRect
    -- purely for symmetry with the other phases.
    { name = "clipped_fill", draw = function()
        disp.clear(RED)
        disp.setClipRect(CX, CY, CW, CH)
        disp.fillRect(FX, FY, FW, FH, BLUE)
        disp.clearClipRect()
        disp.flush()
    end },

    -- After clearClipRect the same fill must paint the whole rect again.
    { name = "clip_cleared", draw = function()
        disp.clear(RED)
        disp.setClipRect(CX, CY, CW, CH)
        disp.clearClipRect()
        disp.fillRect(FX, FY, FW, FH, BLUE)
        disp.flush()
    end },

    -- Known-black presented frame for the partial-flush phases.
    { name = "flush_setup", draw = function()
        disp.clear(BLACK)
        disp.flush()
    end },

    -- Draw buffer goes all-green but ONLY rows 100..131 may reach the screen.
    { name = "flush_partial", draw = function()
        disp.clear(GREEN)
        disp.flushRows(100, 131)
    end },

    -- flushRows must not have swapped: a plain flush now presents the very
    -- same all-green draw buffer.
    { name = "flush_full", draw = function()
        disp.flush()
    end },

    -- flushRegion presents only its band (and swaps + back-syncs internally).
    -- The draw buffer here is the flush_setup one (all black), so paint the
    -- band magenta; rows outside the band must keep the green from flush_full.
    { name = "flush_region", draw = function()
        disp.fillRect(0, 0, 320, 50, MAGENTA)
        disp.flushRegion(0, 49)
    end },
}

local idx = 1

local function draw_phase()
    phases[idx].draw()
    pc.sys.log(("CR:PHASE %d %s"):format(idx, phases[idx].name))
end

draw_phase()

while true do
    input.update()
    local pressed = input.getButtonsPressed()

    if pressed & input.BTN_ESC ~= 0 then
        pc.sys.log("CR:DONE")
        return
    end

    if pressed & input.BTN_ENTER ~= 0 then
        idx = idx + 1
        if idx > #phases then
            pc.sys.log("CR:DONE")
            return
        end
        draw_phase()
    end

    pc.sys.sleep(16)
end
