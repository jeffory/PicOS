-- Effects Test — regression fixture for picocalc.display.applyEffect
--
-- Exists because all ten display_effect_* functions were no-op stubs in the
-- simulator (simulator/stubs/driver_stubs.c) while working on hardware, so the
-- entire applyEffect family silently did nothing in the sim. This fixture is
-- the check that catches that class of regression: it paints a known colour,
-- applies one effect, and holds the frame so the harness can probe pixels.
--
-- Driven by phase: each ENTER advances one effect. The app logs the phase name
-- before holding, so a test can wait_for_log then get_pixel.

local pc    = picocalc
local disp  = pc.display
local input = pc.input

-- Test colour is deliberately NOT grey: a grey input is a fixed point of
-- grayscale, which would make that effect untestable.
local TEST_R, TEST_G, TEST_B = 200, 60, 30
local TEST_COLOR = disp.rgb(TEST_R, TEST_G, TEST_B)

-- Probe points. Row parity matters for scanline: it darkens ODD rows only, so
-- an even/odd pair distinguishes a working scanline from a no-op.
local PROBE_EVEN_Y = 100   -- even row: untouched by scanline
local PROBE_ODD_Y  = 101   -- odd row:  darkened by scanline
local PROBE_X      = 160

local phases = {
    { name = "baseline",   apply = function() end },
    { name = "invert",     apply = function() disp.applyEffect("invert") end },
    { name = "darken",     apply = function() disp.applyEffect("darken", 128) end },
    { name = "brighten",   apply = function() disp.applyEffect("brighten", 128) end },
    { name = "tint",       apply = function() disp.applyEffect("tint", 0, 255, 255, 160) end },
    { name = "grayscale",  apply = function() disp.applyEffect("grayscale") end },
    { name = "dither",     apply = function() disp.applyEffect("dither", 4) end },
    { name = "scanline",   apply = function() disp.applyEffect("scanline", 64) end },
    { name = "posterize",  apply = function() disp.applyEffect("posterize", 3) end },
    -- image.new returns a zeroed (black) image, so blending it at alpha 128
    -- halves the frame. Not a pretty effect, but it is a measurable one and it
    -- proves the blend path reaches the framebuffer.
    { name = "blend",      apply = function()
            local img = pc.graphics.image.new(320, 320)
            disp.applyEffect("blend", img, 128)
        end },
    { name = "scanline255", apply = function() disp.applyEffect("scanline", 255) end },
}

pc.sys.log(("FX:TESTCOLOR r=%d g=%d b=%d"):format(TEST_R, TEST_G, TEST_B))
pc.sys.log(("FX:PROBE x=%d even_y=%d odd_y=%d"):format(PROBE_X, PROBE_EVEN_Y, PROBE_ODD_Y))
pc.sys.log(("FX:PHASES n=%d"):format(#phases))

local idx = 1

local function draw_phase()
    local p = phases[idx]
    disp.clear(TEST_COLOR)
    p.apply()
    -- No text drawn: any glyph would contaminate the probe points, and the
    -- effect has already been applied to the whole frame anyway.
    disp.flush()
    pc.sys.log(("FX:PHASE %d %s"):format(idx, p.name))
end

draw_phase()

while true do
    input.update()
    local pressed = input.getButtonsPressed()

    if pressed & input.BTN_ESC ~= 0 then
        pc.sys.log("FX:DONE")
        return
    end

    if pressed & input.BTN_ENTER ~= 0 then
        idx = idx + 1
        if idx > #phases then
            pc.sys.log("FX:DONE")
            return
        end
        draw_phase()
    end

    pc.sys.sleep(16)
end
