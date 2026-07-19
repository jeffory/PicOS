-- Color verification test fixture
-- Draws solid color blocks at known coordinates for pixel-level verification.
-- E2E tests use get_pixel RPC to verify exact color values.

local pc = picocalc
local d = pc.display
local log = pc.sys.log

-- Phase 1: Solid color blocks in known regions
-- Red block: x=0-99, y=0-99
d.clear(d.BLACK)
d.fillRect(0, 0, 100, 100, d.RED)
-- Green block: x=110-209, y=0-99
d.fillRect(110, 0, 100, 100, d.GREEN)
-- Blue block: x=220-319, y=0-99
d.fillRect(220, 0, 100, 100, d.BLUE)
-- White block: x=0-99, y=110-209
d.fillRect(0, 110, 100, 100, d.WHITE)
-- Yellow (red+green): x=110-209, y=110-209
d.fillRect(110, 110, 100, 100, d.YELLOW)
-- Black stays as background: x=220-319, y=110-209
d.flush()
log("COLOR_BLOCKS_READY")

pc.sys.sleep(200)

-- Phase 2: Full screen red for uniform color check
d.clear(d.RED)
d.flush()
log("COLOR_FULL_RED")

pc.sys.sleep(200)

-- Phase 3: Full screen green
d.clear(d.GREEN)
d.flush()
log("COLOR_FULL_GREEN")

pc.sys.sleep(200)

-- Phase 4: Full screen blue
d.clear(d.BLUE)
d.flush()
log("COLOR_FULL_BLUE")

pc.sys.sleep(100)
log("COLOR_TESTS_DONE")
