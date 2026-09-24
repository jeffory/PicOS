"""Graphics object lifetimes (review: Graphics Critical rows 1-2, High
"addWallSprites ... lua_checkstack", Medium "sprite size larger than the
image"; Task 7 review: raw-pointer acceptance in lua_bridge_graphics.c).

gfx_lifetimes runs each case alone in its own simulator, under the default
collector and under a very aggressive incremental one (stress.flag): before
the fixes most cases are a use-after-free or a heap overflow, and one case's
crash must not cost the others their result. The ASan simulator reports the
bad access itself; the release simulator sees the weak-table survival checks
and the identity checks.

test_lifetimes.py keeps the older gc_test cases for the same bugs.
"""

import pytest

from helpers import case_params, lua_case_names, stage_lua_app

APP = "gfx_lifetimes"
CASES = lua_case_names(APP)

# Known bugs still open, {case: reason}. Each fix removes its entries.
# collected_blinker_leaves_updateAll is not listed: it passes in the release
# simulator (the stale write lands in freed memory unseen) and only the ASan
# simulator reports it (heap-use-after-free in l_animation_blinker_updateAll).
OVERSIZE = ("review: Graphics Medium — sprite width/height are used as the "
            "source stride; Task 10 item 5")
KNOWN_BUGS = {
    "oversize_sprite_reads_only_its_image": OVERSIZE,
}


def _setup(case, mode):
    def setup(sd):
        app = sd / "apps" / APP
        (app / "only.flag").write_text(case)
        if mode == "stress":
            (app / "stress.flag").write_text("1")
    return setup


@pytest.mark.parametrize("mode", ["normal", "stress"])
@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_gfx_lifetime(lua_suite, mode, case):
    run = lua_suite(APP, setup=_setup(case, mode))
    run.check_case(case)
    run.assert_clean_exit()


# ── (a) and the Medium row, on screen ───────────────────────────────────────

# An 8x8 red image on a sprite whose size is set to 24x24, added from a local
# that is dropped before ten collections. sprite.update() must draw the
# image (red at its centre) and nothing past the image's own 8x8 (black
# inside the sprite's larger bounds).
RENDER_APP = r"""
local pc, gfx, d = picocalc, picocalc.graphics, picocalc.display
local function bmp(w, h, colour)
    local pixels = string.rep(string.pack("<I2", colour), w * h)
    local header = string.pack("<c2I4I2I2I4", "BM", 54 + #pixels, 0, 0, 54)
    local info = string.pack("<I4i4i4I2I2I4I4i4i4I4I4",
        40, w, -h, 1, 16, 0, #pixels, 2835, 2835, 0, 0)
    return header .. info .. pixels
end
do
    local s = gfx.sprite.new(gfx.image.loadFromBuffer(bmp(8, 8, 0xF800)))
    s:setSize(24, 24)
    s:moveTo(100, 100)
    s:add()
end
for _ = 1, 10 do collectgarbage("collect") end
local junk = {}
for i = 1, 1000 do junk[i] = string.rep("j", 1000) .. i end
junk = nil
collectgarbage("collect")
d.clear(d.BLACK)
gfx.sprite.update()
d.flush()
pc.sys.log("GLR READY count=" .. gfx.sprite.spriteCount())
while true do
    pc.input.update()
    if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end
    pc.sys.sleep(16)
end
"""

RENDER_BUG = OVERSIZE   # None once it is fixed


def _near(px, want, tol=32):
    return all(abs(a - b) <= tol for a, b in zip((px["r"], px["g"], px["b"]), want))


@pytest.mark.parametrize("_", [pytest.param(
    0, id="render", marks=[pytest.mark.xfail(strict=True, reason=RENDER_BUG)]
    if RENDER_BUG else [])])
def test_dropped_sprite_renders_at_image_size(simulator, _):
    stage_lua_app(simulator.sd_card_path, "gfx_render", RENDER_APP)
    simulator.clear_log()
    simulator.launch_app("gfx_render")
    simulator.wait_for_log("GLR READY", timeout=20)
    texts = [e if isinstance(e, str) else e.get("text", "")
             for e in simulator.get_log_buffer()["lines"]]
    assert any("GLR READY count=1" in t for t in texts), texts
    centre = simulator.call("get_pixel", {"x": 104, "y": 104})
    assert _near(centre, (248, 0, 0)), f"image centre: {centre}"
    for x, y in [(110, 104), (104, 110), (120, 120)]:
        px = simulator.call("get_pixel", {"x": x, "y": y})
        assert _near(px, (0, 0, 0)), f"({x},{y}) past the 8x8 image: {px}"
    simulator.keypress("esc")
    simulator.wait_for_exit(timeout=10)
