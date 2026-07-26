"""E2E tests for the SDK additions built alongside the nonogram app.

Each test drives one of the fixture apps under tests/e2e/apps/ and asserts on
marker log lines plus get_pixel probes, following the house pattern in
test_display.py.

The applyEffect test is the important one: all ten display_effect_* functions
were no-op stubs in the simulator while working on hardware, so the entire
family silently did nothing in the sim. Nothing caught that until this existed.
"""
import time

import pytest


# Injected buttons are HELD for 80ms so extra polls cannot consume them, which
# means two presses of the same key inside that window coalesce into a single
# edge. Any test that taps the same key repeatedly must space the taps past it.
BUTTON_HOLD_S = 0.13


def _lines(sim):
    out = []
    for line in sim.get_log_buffer()["lines"]:
        out.append(line if isinstance(line, str) else line.get("text", ""))
    return out


def _tap(sim, key, count=1):
    for _ in range(count):
        sim.keypress(key)
        time.sleep(BUTTON_HOLD_S)


def _px(sim, x, y):
    return sim.call("get_pixel", {"x": x, "y": y})["rgb565"]


# ── picocalc.json ────────────────────────────────────────────────────────────


def test_json_module(simulator):
    """json.encode/decode round-trips, and game.save keeps nested tables.

    game.save's old encoder emitted `null` for any non-scalar value, so a table
    with nested fields was written out as unrecoverable data loss.
    """
    simulator.clear_log()
    simulator.launch_app("json_test")
    simulator.wait_for_log("JT:DONE", timeout=30)

    lines = _lines(simulator)
    done = [l for l in lines if "JT:DONE" in l]
    assert done, "json_test never reported JT:DONE"

    failures = [l for l in lines if "FAIL" in l and l.strip().startswith("JT:")]
    assert not failures, "json fixture failures:\n" + "\n".join(failures)

    assert "fail=0" in done[-1], done[-1]


# ── applyEffect parity ───────────────────────────────────────────────────────


EFFECT_PHASES = [
    "baseline", "invert", "darken", "brighten", "tint", "grayscale",
    "dither", "scanline", "posterize", "blend", "scanline255",
]


def test_apply_effect_mutates_framebuffer(simulator):
    """Every applyEffect name must actually change pixels in the simulator.

    Probes an even and an odd row: scanline darkens ODD rows only, and that
    parity is the byte-order-sensitive part of the port (the simulator
    framebuffer is host order, the hardware one is byte-swapped).
    """
    simulator.clear_log()
    simulator.launch_app("effects_test")
    simulator.wait_for_log("FX:PHASE 1 baseline", timeout=30)
    time.sleep(0.3)

    seen = {}
    for i in range(len(EFFECT_PHASES)):
        phase = None
        for line in reversed(_lines(simulator)):
            if "FX:PHASE" in line:
                phase = line.split("FX:PHASE", 1)[1].strip()
                break
        assert phase, "no FX:PHASE marker"
        name = phase.split(None, 1)[1]
        seen[name] = (_px(simulator, 160, 100), _px(simulator, 160, 101))
        if i < len(EFFECT_PHASES) - 1:
            _tap(simulator, "enter")
            time.sleep(0.25)

    base_even = seen["baseline"][0]
    assert seen["baseline"][0] == seen["baseline"][1], "baseline rows differ"

    # Whole-frame effects must change the even row too.
    for name in ("invert", "darken", "brighten", "tint", "grayscale",
                 "dither", "posterize", "blend"):
        assert name in seen, f"phase {name} missing"
        assert seen[name][0] != base_even, (
            f"applyEffect('{name}') did not change the framebuffer "
            f"(still 0x{base_even:04X}) — is it a no-op stub?"
        )

    # Scanline leaves even rows alone and darkens odd rows.
    for name in ("scanline", "scanline255"):
        even, odd = seen[name]
        assert even == base_even, f"{name} altered an even row"
        assert odd != even, f"{name} did not darken the odd row"

    assert seen["scanline255"][1] == 0x0000, "scanline 255 should black odd rows"


# ── transparent text and primitives ──────────────────────────────────────────


def _region(sim, x, y, w, h):
    res = sim.call("get_pixel", {"x": x, "y": y, "w": w, "h": h})
    px = res.get("pixels") or []
    flat = []
    if px and isinstance(px[0], list):
        for row in px:
            flat.extend(row)
    else:
        flat = px
    return [v if isinstance(v, int) else v.get("rgb565") for v in flat]


DP_PHASES = [
    "backdrop", "text_opaque", "text_transparent", "text_default_bg",
    "text_transparent_sci", "fill_hline", "fill_hline_reversed", "fill_triangle",
]

RED, BLUE, WHITE, GREEN, BLACK = 0xF800, 0x001F, 0xFFFF, 0x07E0, 0x0000


def test_transparent_text_and_primitives(simulator):
    """bg=false must leave non-glyph pixels untouched; fillHLine/fillTriangle work."""
    simulator.clear_log()
    simulator.launch_app("drawprim_test")
    simulator.wait_for_log("DP:PHASE 1 backdrop", timeout=30)
    time.sleep(0.3)

    seen = {}
    for i in range(len(DP_PHASES)):
        phase = None
        for line in reversed(_lines(simulator)):
            if "DP:PHASE" in line:
                phase = line.split("DP:PHASE", 1)[1].strip()
                break
        assert phase, "no DP:PHASE marker"
        name = phase.split(None, 1)[1]
        seen[name] = _region(simulator, 64, 64, 16, 16)
        if i < len(DP_PHASES) - 1:
            _tap(simulator, "enter")
            time.sleep(0.25)

    assert set(seen["backdrop"]) == {RED}, "backdrop not a clean fill"

    opaque = seen["text_opaque"]
    assert opaque.count(BLUE) > 0, "opaque text did not paint its background"
    assert opaque.count(WHITE) > 0, "opaque text drew no glyph"

    trans = seen["text_transparent"]
    assert trans.count(BLUE) == 0, (
        "transparent text still wrote a background colour"
    )
    assert trans.count(RED) > 0, "transparent text erased the backdrop"
    assert trans.count(WHITE) == opaque.count(WHITE), (
        "transparent glyph differs from the opaque glyph"
    )

    # An absent bg argument must keep the original opaque-black behaviour.
    assert seen["text_default_bg"].count(BLACK) > 0, (
        "absent bg no longer defaults to opaque black — backward incompatible"
    )

    sci = seen["text_transparent_sci"]
    assert sci.count(BLUE) == 0 and sci.count(RED) > 0 and sci.count(WHITE) > 0, (
        "transparent mode broken in the 6x12 font branch"
    )

    assert seen["fill_hline"].count(GREEN) == 16, "fillHLine wrong span"
    assert seen["fill_hline_reversed"].count(GREEN) == 16, (
        "fillHLine did not normalise reversed x0/x1"
    )
    tri = seen["fill_triangle"].count(GREEN)
    assert 0 < tri < 256, f"fillTriangle filled {tri}/256 pixels"


# ── clip rect + partial flush ────────────────────────────────────────────────


CR_PHASES = [
    "backdrop", "clipped_fill", "clip_cleared",
    "flush_setup", "flush_partial", "flush_full", "flush_region",
]

CR_RED, CR_BLUE, CR_GREEN = 0xF800, 0x001F, 0x07E0
CR_BLACK, CR_MAGENTA = 0x0000, 0xF81F

# Fixed probe points, sampled at every phase.  The fixture's clip rect is
# (64,64,32,32) and its fill rect (48,48,64,64) crosses all four clip edges.
CR_PROBES = {
    "in_clip":      (80, 80),    # inside clip AND inside fill rect
    "left_of_clip": (56, 80),    # inside fill rect, left of the clip
    "below_clip":   (80, 100),   # inside fill rect, below the clip
    "outside_fill": (200, 200),  # outside everything
    "rows_above":   (160, 50),   # above the flushRows band (100..131)
    "rows_mid":     (160, 110),  # inside the flushRows band
    "rows_below":   (160, 200),  # below the flushRows band
    "region_top":   (160, 25),   # inside the flushRegion band (0..49)
}


def test_clip_rect_and_partial_flush(simulator):
    """setClipRect must confine draws; flushRows/flushRegion must present only
    their rows.  All three were prime candidates for silent sim no-ops (the
    stub re-implements the whole display driver), hence the pixel probes
    against the PRESENTED framebuffer rather than log markers alone."""
    simulator.clear_log()
    simulator.launch_app("cliprect_test")
    simulator.wait_for_log("CR:PHASE 1 backdrop", timeout=30)
    time.sleep(0.3)

    seen = {}
    for i in range(len(CR_PHASES)):
        phase = None
        for line in reversed(_lines(simulator)):
            if "CR:PHASE" in line:
                phase = line.split("CR:PHASE", 1)[1].strip()
                break
        assert phase, "no CR:PHASE marker"
        name = phase.split(None, 1)[1]
        seen[name] = {
            label: _px(simulator, px, py)
            for label, (px, py) in CR_PROBES.items()
        }
        if i < len(CR_PHASES) - 1:
            _tap(simulator, "enter")
            time.sleep(0.25)

    for name in CR_PHASES:
        assert name in seen, f"phase {name} missing"

    # getClipRect: an oversized rect clamps to the framebuffer, a normal one
    # round-trips exactly.
    lines = _lines(simulator)
    assert any("CR:CLIPFULL 0 0 320 320" in l for l in lines), (
        "setClipRect(-16,-16,400,400) did not clamp to 0,0,320,320"
    )
    assert any("CR:CLIP 64 64 32 32" in l for l in lines), (
        "getClipRect did not round-trip the set clip"
    )

    assert seen["backdrop"]["in_clip"] == CR_RED, "backdrop not a clean fill"
    assert seen["backdrop"]["left_of_clip"] == CR_RED

    # Clipped fill: blue only inside the clip; the parts of the fill rect
    # outside the clip stay red.
    cf = seen["clipped_fill"]
    assert cf["in_clip"] == CR_BLUE, (
        "fillRect inside the clip did not draw — clip suppressed everything?"
    )
    assert cf["left_of_clip"] == CR_RED, (
        "fillRect leaked LEFT of the clip — setClipRect is a no-op?"
    )
    assert cf["below_clip"] == CR_RED, (
        "fillRect leaked BELOW the clip — setClipRect is a no-op?"
    )
    assert cf["outside_fill"] == CR_RED

    # After clearClipRect, the same fill paints the whole rect.
    cc = seen["clip_cleared"]
    assert cc["in_clip"] == CR_BLUE
    assert cc["left_of_clip"] == CR_BLUE, (
        "clearClipRect did not restore full drawing"
    )
    assert cc["below_clip"] == CR_BLUE

    # flushRows: draw buffer is all green, but only rows 100..131 were
    # flushed — everything else must still show the black setup frame.
    assert seen["flush_setup"]["rows_mid"] == CR_BLACK
    fp = seen["flush_partial"]
    assert fp["rows_mid"] == CR_GREEN, (
        "flushRows did not present the requested rows"
    )
    assert fp["rows_above"] == CR_BLACK, (
        "flushRows presented rows ABOVE the requested band"
    )
    assert fp["rows_below"] == CR_BLACK, (
        "flushRows presented rows BELOW the requested band"
    )

    # flushRows must not swap: the very next flush() presents the same
    # all-green draw buffer.
    ff = seen["flush_full"]
    assert ff["rows_above"] == CR_GREEN and ff["rows_below"] == CR_GREEN, (
        "flush after flushRows lost the draw buffer — flushRows swapped?"
    )

    # flushRegion: only rows 0..49 change; the rest keeps the green frame.
    fr = seen["flush_region"]
    assert fr["region_top"] == CR_MAGENTA, "flushRegion did not present its band"
    assert fr["rows_mid"] == CR_GREEN and fr["rows_below"] == CR_GREEN, (
        "flushRegion presented rows outside its band"
    )


# ── input auto-repeat ────────────────────────────────────────────────────────


def test_input_auto_repeat(simulator):
    """A held button must yield one press edge but many repeated edges."""
    simulator.clear_log()
    simulator.launch_app("repeat_test")
    simulator.wait_for_log("RP:READY", timeout=30)
    time.sleep(0.2)

    simulator.call("inject_button", {"button": "right", "action": "press"})
    time.sleep(1.0)
    simulator.call("inject_button", {"button": "right", "action": "release"})
    time.sleep(0.3)
    simulator.keypress("esc")
    simulator.wait_for_log("RP:DONE", timeout=15)

    lines = _lines(simulator)
    presses = [l for l in lines if "RP:PRESS" in l]
    repeats = [l for l in lines if "RP:REPEAT" in l]

    assert len(presses) == 1, f"expected 1 press edge, got {len(presses)}"
    assert len(repeats) >= 5, f"expected >=5 repeated edges, got {len(repeats)}"
    assert len(repeats) > len(presses)

    result = [l for l in lines if "RP:RESULT" in l]
    assert result, "no RP:RESULT line"
    measured = result[-1].split("delay_measured=")[1].split()[0]
    assert measured != "nil", "never measured a repeat delay"
    # Configured 200ms, sampled on a 16ms frame loop.
    assert 180 <= int(measured) <= 320, f"repeat delay {measured}ms out of range"
