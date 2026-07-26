-- scrollreg_test — pixel-probe fixture for hardware vertical scroll.
--
-- Phases advance on ENTER and log "SR:PHASE <n> <name>"; between phases the
-- test probes PRESENTED pixels.  The interesting semantics:
--
--   * setScrollArea(0, 320, 160) makes the visible panel a mod-320 ring:
--     screen row L shows GRAM row (offset + L) % 320.
--   * setScrollOffset remaps instantly with no flush.
--   * flushRows writes GRAM rows (= framebuffer rows), NOT screen rows —
--     under a nonzero offset the flushed band appears where the ring says.
--   * getScrollOffset returns offset plus a write counter that increments
--     on every register write (foreign-write detection for apps).
--
-- Ten 32px bands of known RGB565 colours; the test knows the same table.

local disp  = picocalc.display
local input = picocalc.input
local sys   = picocalc.sys

local SCREEN = 320

local BANDS = {
    0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F,
    0x07FF, 0x8410, 0xFFFF, 0x4208, 0x8000,
}
local STRIP = 0xFD20  -- orange, distinct from every band colour

local function waitEnter()
    while true do
        input.update()
        local p = input.getButtonsPressed()
        if p & input.BTN_ENTER ~= 0 then return true end
        if p & input.BTN_ESC ~= 0 then return false end
        sys.sleep(15)
    end
end

-- Phase 1: flat pattern, identity scroll.
for i = 1, 10 do
    disp.fillRect(0, (i - 1) * 32, SCREEN, 32, BANDS[i])
end
disp.flush()
sys.log("SR:PHASE 1 flat")
if not waitEnter() then return end

-- Phase 2: ring the panel, offset 64.  Screen row L -> GRAM (64+L)%320.
disp.setScrollArea(0, SCREEN, 160)
disp.setScrollOffset(64)
local off, gen1 = disp.getScrollOffset()
disp.setScrollOffset(64)  -- same value: the write counter must still bump
local _, gen2 = disp.getScrollOffset()
sys.log("SR:OFF " .. tostring(off)
        .. " GENDELTA " .. tostring((gen2 or 0) - (gen1 or 0)))
sys.log("SR:PHASE 2 scrolled")
if not waitEnter() then return end

-- Phase 3: flushRows targets GRAM rows.  Paint framebuffer rows 0..7 and
-- flush them; under offset 64 they must appear at screen rows 256..263.
disp.fillRect(0, 0, SCREEN, 8, STRIP)
disp.flushRows(0, 7)
sys.log("SR:PHASE 3 strip")
if not waitEnter() then return end

-- Phase 4: reset to identity; the strip now shows at its GRAM home (rows
-- 0..7) and the wrap disappears.
disp.setScrollOffset(0)
sys.log("SR:PHASE 4 reset")
waitEnter()
