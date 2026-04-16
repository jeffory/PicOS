-- Display drawing test fixture
-- Draws known patterns for E2E screenshot verification.
-- Logs markers so E2E can wait for drawing to complete.

local pc = picocalc
local d = pc.display
local log = pc.sys.log

-- Phase 1: Fill screen with red
d.clear(d.RED)
d.flush()
pc.sys.sleep(100)
log("DRAW:red_fill")

-- Phase 2: Draw a white rectangle in the center
d.clear(d.BLACK)
d.fillRect(100, 100, 120, 120, d.WHITE)
d.flush()
pc.sys.sleep(100)
log("DRAW:white_rect")

-- Phase 3: Draw text
d.clear(d.BLACK)
d.drawText(10, 10, "HELLO E2E", d.GREEN)
d.flush()
pc.sys.sleep(100)
log("DRAW:text")

-- Phase 4: Draw multiple colored rectangles for color verification
d.clear(d.BLACK)
-- Red band (top)
d.fillRect(0, 0, 320, 50, d.RED)
-- Green band (middle)
d.fillRect(0, 100, 320, 50, d.GREEN)
-- Blue band (bottom)
d.fillRect(0, 200, 320, 50, d.BLUE)
d.flush()
pc.sys.sleep(100)
log("DRAW:color_bands")

log("DISPLAY_TESTS_DONE")
pc.sys.sleep(100)
