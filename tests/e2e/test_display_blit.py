"""Blitter clipping, colour key + flip, PNG decode and off-screen shape rejection.

Drives tests/e2e/apps/blit_test phase by phase (ENTER) and probes the screen
with get_pixel. Expected pixels come from an independent Python model of the
blit semantics (clip to the screen, flip, colour key) applied to the fixture
PNGs as PIL decodes them, so a clipping or flip regression in either the
firmware twin or the simulator shows up as a pixel mismatch, not just as a
changed golden.

Regression targets (code review 2026-09-24, Graphics):
- PNGdec's zlib made misaligned 32-bit copies on every real PNG (UBSan abort
  in the ASan simulator on image.load).
- drawImageNN / drawScaledNN with negative, non-multiple offsets.
- No trivial rejection of off-screen shapes: drawLine(0,0,1e9,1e9),
  circles of radius 1e9 and huge triangles looped for ~1e9 pixels.
- draw3DWireframeEx divided by zero for a vertex on the camera plane.
"""

import os
import re

from PIL import Image

APP = "blit_test"
APP_DIR = os.path.join(os.path.dirname(__file__), "apps", APP)

PHASES = ["png", "keyed_flip", "nn_negative", "line_huge", "circle_huge",
          "tri_huge", "cube_behind"]


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


BG = rgb565(0, 0, 80)
INK = rgb565(0, 255, 0)
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


def _ms(simulator, name):
    for line in simulator.get_log_buffer()["lines"]:
        text = line if isinstance(line, str) else line.get("text", "")
        m = re.search(rf"BT:MS {name} (\d+)", text)
        if m:
            return int(m.group(1))
    raise AssertionError(f"no BT:MS {name} line")


def _check(sim, m, spots, what, w=16, h=16):
    for (x, y) in spots:
        got = region(sim, x, y, w, h)
        want = m.region(x, y, w, h)
        assert got == want, f"{what} at ({x},{y}): {diff(got, want, x, y, w)}"


PATTERN = load565("pattern.png")
SPRITE = load565("sprite.png")

# Off-screen shapes must be rejected or clipped before the pixel loop. The
# unfixed loops ran ~1e9 iterations (minutes); a clipped one is microseconds.
PROMPT_MS = 1000


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


def test_nn_scale_negative_offset(simulator):
    """Integer NN scale starting at a negative, non-multiple offset."""
    _goto(simulator, "nn_negative")
    m = Model()
    m.blit_nn(-3, -5, SPRITE, 4, key=KEY)
    m.blit_nn(300, 310, SPRITE, 3, key=KEY)
    _check(simulator, m, [(0, 0), (16, 0), (0, 16), (304, 304)], "nn")


def test_huge_lines_clipped(simulator):
    _goto(simulator, "line_huge")
    m = Model()
    for k in range(320):
        m.px[k * 320 + k] = INK          # (0,0)->(1e9,1e9): the diagonal
        m.px[50 * 320 + k] = INK         # y = 50 across the whole screen
    _check(simulator, m, [(0, 0), (304, 304), (0, 48), (160, 48), (304, 48)],
           "line")
    assert _ms(simulator, "line_huge") < PROMPT_MS


def test_huge_circles_clipped(simulator):
    _goto(simulator, "circle_huge")
    m = Model()
    for k in range(320):
        m.px[60 * 320 + k] = INK         # outline grazing y = 60
    m.px[200 * 320 + 160] = INK          # fill: apex pixel on row 200
    for y in range(201, 320):
        for k in range(320):
            m.px[y * 320 + k] = INK
    _check(simulator, m, [(0, 52), (152, 52), (304, 52), (0, 192), (152, 192),
                          (304, 304)], "circle")
    assert _ms(simulator, "circle_huge") < PROMPT_MS


def test_huge_triangle_clipped(simulator):
    _goto(simulator, "tri_huge")
    m = Model()
    for y in range(100, 320):
        for k in range(320):
            m.px[y * 320 + k] = INK
    _check(simulator, m, [(0, 92), (304, 92), (0, 304), (304, 304)], "tri")
    assert _ms(simulator, "tri_huge") < PROMPT_MS


def test_3d_vertex_behind_camera(simulator):
    _goto(simulator, "cube_behind")
    # Front face (z = -40, depth 60) top edge: y = 160 - 40*100/60 -> 93.
    got = region(simulator, 104, 93, 16, 1)
    assert got == [INK] * 16, f"cube front edge missing: {got}"
    assert _ms(simulator, "cube_behind") < PROMPT_MS
