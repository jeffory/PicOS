"""Blitter clipping, colour key + flip and PNG decode.

Drives tests/e2e/apps/blit_test phase by phase (ENTER) and probes the screen
with get_pixel. Expected pixels come from an independent Python model of the
blit semantics (clip to the screen, flip, colour key) applied to the fixture
PNGs as PIL decodes them, so a clipping or flip regression in either the
firmware twin or the simulator shows up as a pixel mismatch, not just as a
changed golden.

Regression targets (code review 2026-09-24, Graphics):
- PNGdec's zlib made misaligned 32-bit copies on every real PNG (UBSan abort
  in the ASan simulator on image.load).
"""

import os

from PIL import Image

APP = "blit_test"
APP_DIR = os.path.join(os.path.dirname(__file__), "apps", APP)

PHASES = ["png", "keyed_flip"]


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


BG = rgb565(0, 0, 80)
KEY = rgb565(248, 0, 248)


def load565(name):
    im = Image.open(os.path.join(APP_DIR, name)).convert("RGB")
    w, h = im.size
    px = [rgb565(*im.getpixel((x, y))) for y in range(h) for x in range(w)]
    return w, h, px


class Model:
    """A 320x320 RGB565 screen with the driver's blit semantics."""

    def __init__(self, fill=BG):
        self.px = [fill] * (320 * 320)

    def blit(self, x, y, img, flip_x=False, flip_y=False, key=None):
        w, h, data = img
        for r in range(h):
            for c in range(w):
                sx = w - 1 - c if flip_x else c
                sy = h - 1 - r if flip_y else r
                v = data[sy * w + sx]
                if key is not None and v == key:
                    continue
                dx, dy = x + c, y + r
                if 0 <= dx < 320 and 0 <= dy < 320:
                    self.px[dy * 320 + dx] = v

    def blit_nn(self, x, y, img, scale, key=None):
        w, h, data = img
        for r in range(h * scale):
            for c in range(w * scale):
                v = data[(r // scale) * w + c // scale]
                if key is not None and v == key:
                    continue
                dx, dy = x + c, y + r
                if 0 <= dx < 320 and 0 <= dy < 320:
                    self.px[dy * 320 + dx] = v

    def region(self, x, y, w=16, h=16):
        return [self.px[(y + r) * 320 + x + c] for r in range(h) for c in range(w)]


def region(sim, x, y, w=16, h=16):
    res = sim.call("get_pixel", {"x": x, "y": y, "w": w, "h": h})
    flat = []
    for row in res.get("pixels") or []:
        flat.extend(row if isinstance(row, list) else [row])
    return [v if isinstance(v, int) else v.get("rgb565") for v in flat]


def diff(got, want, x, y, w=16):
    bad = [(x + i % w, y + i // w, g, e)
           for i, (g, e) in enumerate(zip(got, want)) if g != e]
    return ", ".join(f"({bx},{by}) got 0x{g:04X} want 0x{e:04X}"
                     for bx, by, g, e in bad[:6]) + (
        f" … {len(bad)} pixels differ" if len(bad) > 6 else "")


def _goto(simulator, target):
    """Launch the app and ENTER through the phases up to `target`."""
    simulator.clear_log()
    simulator.launch_app(APP)
    simulator.wait_for_log(r"BT:PHASE 1 png", timeout=30)
    for i, name in enumerate(PHASES, start=1):
        if i > 1:
            seq = simulator.keypress("enter")["input_seq"]
            simulator.wait_input_consumed(seq)
            simulator.wait_for_log(rf"BT:PHASE {i} {name}", timeout=30)
        if name == target:
            return  # the app logs BT:PHASE after its flush returned
    raise AssertionError(f"unknown phase {target}")


def _check(sim, m, spots, what, w=16, h=16):
    for (x, y) in spots:
        got = region(sim, x, y, w, h)
        want = m.region(x, y, w, h)
        assert got == want, f"{what} at ({x},{y}): {diff(got, want, x, y, w)}"


PATTERN = load565("pattern.png")
SPRITE = load565("sprite.png")

def test_png_decode_and_clipped_blit(simulator):
    """A real PNG through PNGdec's inflate, drawn opaque and clipped."""
    _goto(simulator, "png")
    m = Model()
    m.blit(0, 0, PATTERN)
    m.blit(-5, 97, PATTERN)
    m.blit(300, 250, PATTERN)
    _check(simulator, m, [(0, 0), (48, 32), (16, 16), (0, 97), (0, 128),
                          (304, 282), (304, 250)], "png")


def test_keyed_flipped_sprite(simulator):
    """Colour key x flip, including sprites clipped on every edge."""
    _goto(simulator, "keyed_flip")
    m = Model()
    m.blit(16, 16, SPRITE, key=KEY)
    m.blit(32, 16, SPRITE, flip_x=True, key=KEY)
    m.blit(48, 16, SPRITE, flip_y=True, key=KEY)
    m.blit(64, 16, SPRITE, flip_x=True, flip_y=True, key=KEY)
    m.blit(-3, 40, SPRITE, flip_x=True, key=KEY)
    m.blit(316, 40, SPRITE, flip_y=True, key=KEY)
    m.blit(16, -2, SPRITE, flip_x=True, key=KEY)
    _check(simulator, m, [(16, 16), (32, 16), (48, 16), (64, 16), (0, 40),
                          (304, 40), (16, 0)], "keyed/flip")
