-- Draw Primitives Test — fixture for transparent text, fillHLine, fillTriangle.
--
-- The load-bearing case is transparent text: display.drawText could only ever
-- paint an opaque background, so text over a gradient or over art was
-- impossible. Passing bg == false must leave non-glyph pixels untouched.
--
-- Lua cannot read pixels back, so this app only *draws* and the harness probes
-- with get_pixel. Each ENTER advances one phase.

local pc    = picocalc
local disp  = pc.display
local input = pc.input

local BACKDROP = disp.rgb(255, 0, 0)     -- red   fill under the text
local TEXTBG   = disp.rgb(0, 0, 255)     -- blue  opaque text background
local TEXTFG   = disp.rgb(255, 255, 255) -- white glyphs
local LINE     = disp.rgb(0, 255, 0)     -- green line / triangle

-- Text is drawn at a 16-aligned spot so the harness can grab one 16x16 region
-- (get_pixel's maximum) that fully contains the glyph cell.
local TX, TY = 64, 64
local PROBE_W, PROBE_H = 16, 16

pc.sys.log(("DP:REGION x=%d y=%d w=%d h=%d"):format(TX, TY, PROBE_W, PROBE_H))
pc.sys.log(("DP:COLORS backdrop=%d textbg=%d textfg=%d line=%d")
    :format(BACKDROP, TEXTBG, TEXTFG, LINE))

local phases = {
    -- Baseline: the backdrop alone, so the harness knows the untouched value.
    { name = "backdrop", draw = function()
        disp.clear(BACKDROP)
    end },

    -- Opaque text: non-glyph pixels inside the cell must become TEXTBG.
    { name = "text_opaque", draw = function()
        disp.clear(BACKDROP)
        disp.setFont(disp.FONT_6X8)
        disp.drawText(TX, TY, "L", TEXTFG, TEXTBG)
    end },

    -- Transparent text: non-glyph pixels must still be BACKDROP, and there
    -- must be no TEXTBG anywhere in the cell.
    { name = "text_transparent", draw = function()
        disp.clear(BACKDROP)
        disp.setFont(disp.FONT_6X8)
        disp.drawText(TX, TY, "L", TEXTFG, false)
    end },

    -- Absent bg must keep the old opaque-black default (backward compatibility).
    { name = "text_default_bg", draw = function()
        disp.clear(BACKDROP)
        disp.setFont(disp.FONT_6X8)
        disp.drawText(TX, TY, "L", TEXTFG)
    end },

    -- Transparent text in the 12px-tall font exercises a different glyph
    -- branch (row-major vs the 6x8's column-major layout).
    { name = "text_transparent_sci", draw = function()
        disp.clear(BACKDROP)
        disp.setFont(disp.FONT_SCIENTIFICA)
        disp.drawText(TX, TY, "L", TEXTFG, false)
        disp.setFont(disp.FONT_6X8)
    end },

    { name = "fill_hline", draw = function()
        disp.clear(BACKDROP)
        -- Row TY+4 only, spanning the probe region.
        disp.fillHLine(TY + 4, TX, TX + PROBE_W - 1, LINE)
    end },

    -- Reversed arguments must draw the same span, not nothing.
    { name = "fill_hline_reversed", draw = function()
        disp.clear(BACKDROP)
        disp.fillHLine(TY + 4, TX + PROBE_W - 1, TX, LINE)
    end },

    -- A triangle covering the lower-left of the probe region: (TX,TY+15) is
    -- inside, (TX+15,TY) is outside.
    { name = "fill_triangle", draw = function()
        disp.clear(BACKDROP)
        disp.fillTriangle(TX, TY, TX, TY + 15, TX + 15, TY + 15, LINE)
    end },
}

local idx = 1

local function draw_phase()
    phases[idx].draw()
    disp.flush()
    pc.sys.log(("DP:PHASE %d %s"):format(idx, phases[idx].name))
end

draw_phase()

while true do
    input.update()
    local pressed = input.getButtonsPressed()

    if pressed & input.BTN_ESC ~= 0 then
        pc.sys.log("DP:DONE")
        return
    end

    if pressed & input.BTN_ENTER ~= 0 then
        idx = idx + 1
        if idx > #phases then
            pc.sys.log("DP:DONE")
            return
        end
        draw_phase()
    end

    pc.sys.sleep(16)
end
