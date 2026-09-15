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
