"""Font rendering tests.

Pages 0-3 of the font_test fixture are compared byte-for-byte against PNGs in
tests/e2e/fixtures/fonts/. Do not regenerate them casually: they are the proof
that the shared renderer in src/fonts/ reproduces the blitters it replaced.

Pages 2 and 3 (scientifica, scientifica-bold) are byte-exact captures of the
renderer that shipped in develop @bab923e, and must stay that way.

Pages 0 and 1 (6x8, 8x12) were re-captured when the display driver moved onto
the shared renderer. Until then the simulator carried its own copies of both
glyph tables, and they were not copies: the 6x8 differed from the firmware's in
four glyphs ('<', '>', 'M', 'g') and the 8x12 was a different typeface
altogether. The firmware tables are what the device renders, so they are now
the single source for firmware and simulator alike, and the old page 0/1
goldens recorded simulator-only output that no build can produce any more.
"""
import time
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

BUTTON_HOLD_S = 0.13
GOLDEN_DIR = Path("tests/e2e/fixtures/fonts")
BUILTIN_NAMES = ["6x8", "8x12", "scientifica", "scientifica-bold"]


def _lines(sim):
    return [
        l if isinstance(l, str) else l.get("text", "")
        for l in sim.get_log_buffer()["lines"]
    ]


def _goto_page(sim, n, current):
    """Advance the fixture from page `current` to page `n` with Right taps."""
    for _ in range(n - current):
        sim.keypress("right")
        time.sleep(BUTTON_HOLD_S)
    sim.wait_for_log(f"FT:PAGE {n} ", timeout=10)
    time.sleep(0.2)  # let the flush land before screenshotting


def _screenshot_array(sim):
    return np.array(sim.screenshot_pil().convert("RGB"))


@pytest.mark.parametrize("page", [0, 1, 2, 3])
def test_builtin_font_golden(simulator, update_baselines, page):
    simulator.clear_log()
    simulator.launch_app("font_test")
    simulator.wait_for_log("FT:PAGE 0 ", timeout=30)
    _goto_page(simulator, page, 0)

    got = _screenshot_array(simulator)
    golden_path = GOLDEN_DIR / f"builtin_{page}.png"
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    if update_baselines or not golden_path.exists():
        Image.fromarray(got).save(golden_path)
        pytest.skip(f"golden written: {golden_path}")

    want = np.array(Image.open(golden_path).convert("RGB"))
    assert want.shape == got.shape
    diff = np.argwhere(np.any(want != got, axis=-1))
    assert diff.size == 0, (
        f"page {page} differs from golden in {len(diff)} pixels, "
        f"first at (x={diff[0][1]}, y={diff[0][0]})"
    )


def test_builtin_width_matches_measure(simulator):
    """drawText's return value and textWidth agree for every built-in."""
    simulator.clear_log()
    simulator.launch_app("font_test")
    simulator.wait_for_log("FT:PAGE 0 ", timeout=30)
    for page in range(1, 4):
        _goto_page(simulator, page, page - 1)
    widths = [l for l in _lines(simulator) if l.startswith("FT:WIDTH ")]
    assert len(widths) == 4, widths
    for line in widths:
        _, name, drawn, measured = line.split()
        assert drawn == measured, line
        assert name in BUILTIN_NAMES


def test_extended_and_loaded_pages(simulator):
    simulator.clear_log()
    simulator.launch_app("font_test")
    simulator.wait_for_log("FT:PAGE 0 ", timeout=30)
    for page in range(1, 5):
        _goto_page(simulator, page, page - 1)

    # Page 4 (extended) is on screen: probe it before page 5 clears the frame.
    # Third line, drawn at (2,30) in scientifica-bold (6px monospace cells):
    # "a" @2, "\127" @8, "b" @14, "\1" @20, "c" @26. 0x01 is outside the
    # face's 0x20..0x9F range, so it renders as the hollow fallback box whose
    # top-left pixel is foreground.
    px = simulator.call("get_pixel", {"x": 20, "y": 30})["rgb565"]
    assert px == 0xFFFF, hex(px)
    # 0x7F *is* inside that range (a blank glyph), so it must not draw a box.
    px = simulator.call("get_pixel", {"x": 8, "y": 30})["rgb565"]
    assert px == 0x0000, hex(px)

    _goto_page(simulator, 5, 4)

    lines = _lines(simulator)
    load = [l for l in lines if l.startswith("FT:LOAD ")]
    assert load and load[0].split()[1] == "4", load        # first loadable slot
    prop = [l for l in lines if l.startswith("FT:PROP ")][0].split()
    assert int(prop[1]) < int(prop[2]), prop                 # 'i' narrower than 'W'
    unload = [l for l in lines if l.startswith("FT:UNLOAD ")]
    assert unload and unload[0].split()[1] == "0", unload    # self-heal to the 6x8 built-in
    obj = [l for l in lines if l.startswith("FT:FONTOBJ ")][0].split()
    assert obj[1].endswith("demo_prop.pfn") and obj[2] == "8" and obj[3] == "12"
    assert int(obj[4]) == int(prop[1])                       # font obj measures like display
    size = [l for l in lines if l.startswith("FT:SIZE ")][0].split()
    assert int(size[1]) == int(prop[1]) and int(size[2]) == 12
    assert any(l == "FT:BADLOAD nil" for l in lines), lines
    assert any(l == "FT:BADNEW false" for l in lines), lines


def test_font_state_reset_between_apps(simulator):
    """A font selected and loaded by one app must not leak into the next.

    The fixture exits dirty on purpose: it selects 8x12 and takes a raw font
    slot via display.loadFont, which is a plain integer no Lua GC can reclaim
    (a graphics.font object would be freed by lua_close and hide the bug). So
    the only thing that can hand slot 4 back, or put the selection back to 0,
    is display_set_font(0) / font_registry_unload_all() in launcher.c.
    """
    simulator.clear_log()
    simulator.launch_app("font_test")
    simulator.wait_for_log("FT:PAGE 0 ", timeout=30)
    for page in range(1, 6):
        _goto_page(simulator, page, page - 1)
    simulator.keypress("esc")
    # The simulator has no wait_for_exit RPC; the fixture logs FT:DONE as it
    # returns to the launcher, which is what runs the font-state reset.
    simulator.wait_for_log("FT:DONE", timeout=10)
    time.sleep(0.5)
    exits = [l for l in _lines(simulator) if l.startswith("FT:EXIT ")]
    assert exits == ["FT:EXIT font=1 leak=4"], exits

    simulator.clear_log()
    simulator.launch_app("font_test")
    simulator.wait_for_log("FT:PAGE 0 ", timeout=30)
    entry = [l for l in _lines(simulator) if l.startswith("FT:ENTRY ")]
    assert entry and entry[0] == "FT:ENTRY 0", (
        f"the 8x12 selection leaked into the next app: {entry}")
    for page in range(1, 6):
        _goto_page(simulator, page, page - 1)
    load = [l for l in _lines(simulator) if l.startswith("FT:LOAD ")]
    assert load[0].split()[1] == "4", "slot 4 was not freed by the launcher"
