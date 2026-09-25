-- theme.lua — the only place colours, fonts and chrome metrics are defined.
--
-- Terminal / ICE-breaker direction: phosphor green on black, magenta for
-- alerts. Deliberately NOT dependent on picocalc.* so the pure-logic test
-- harness can load it on a host Lua.

local T = {}

-- RGB565 in host byte order. Identical formula to picocalc.display.rgb, but
-- pure Lua so this module stays host-loadable.
local function rgb(r, g, b)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
end
T.rgb = rgb

-- ── Palette ──────────────────────────────────────────────────────────────────

T.BG          = rgb(  0,   0,   0)   -- true black: this is a CRT, not a screen
T.BG_PANEL    = rgb(  6,  18,   8)
T.GRID        = rgb( 10,  48,  16)   -- dim phosphor grid
T.GRID_MAJOR  = rgb( 24,  92,  36)   -- every 5th line, for counting by eye

T.FILL        = rgb( 51, 255, 102)   -- solid phosphor green
T.FILL_EDGE   = rgb(140, 255, 180)   -- 1px lit edge, fakes CRT bloom
T.BLOCK       = rgb( 74,  90,  74)   -- dim grey X
T.MAYBE       = rgb(255, 208,   0)   -- amber ?

T.CLUE        = rgb(160, 255, 190)   -- active clue text
T.CLUE_DONE   = rgb( 46,  74,  52)   -- retired clue: dimmed, not struck out
T.LANE_DONE   = rgb(  8,  40,  16)   -- satisfied-line backlight
T.LANE_OVER   = rgb( 64,  10,  26)   -- over-filled backlight

T.ALERT       = rgb(255,  46, 154)   -- magenta: warnings, destructive actions
T.CURSOR      = rgb(255,  46, 154)
T.TEXT        = rgb(200, 255, 214)
T.TEXT_DIM    = rgb( 90, 130, 100)
T.OK          = rgb( 51, 255, 102)

-- Colour key for sprite transparency. NOT black (a transparent_color of 0 means
-- "disabled" in the driver) and NOT the magenta alert colour, so alert-coloured
-- pixels in art are never punched out by accident.
T.SPRITE_KEY  = rgb(255, 0, 254)

-- ── Chrome metrics ───────────────────────────────────────────────────────────
--
-- 15x15 leaves only a few pixels of vertical slack, so these are hard
-- constraints rather than suggestions — see layout.lua for the arithmetic.

T.HEADER_H  = 18
T.FOOTER_H  = 20
T.GAP       = 2
T.MARGIN_L  = 4
T.MARGIN_R  = 6

T.SCANLINE_INTENSITY = 64   -- 1-127 halves odd rows once

-- Font ids, mirroring picocalc.display.FONT_*. Duplicated as plain numbers so
-- this file has no picocalc dependency; render.lua asserts they still match.
T.FONT_6X8        = 0
T.FONT_8X12       = 1
T.FONT_SCI        = 2
T.FONT_SCI_BOLD   = 3

T.CLUE_FONT   = T.FONT_6X8   -- 6x8: the digit metrics layout.lua assumes
T.CLUE_CHAR_W = 6
T.CLUE_CHAR_H = 8
T.HEADER_FONT = T.FONT_SCI_BOLD

return T
