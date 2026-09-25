"""E2E tests for tolerant integer arguments in the Lua bridge.

lua_Number is float32 (Task 1), so ordinary app arithmetic produces values
such as 199.99998 where double arithmetic used to land on 200. Bridge
functions that took those quantities through luaL_checkinteger then raised
"number has no integer representation" (seen on hardware in Guinea Pig Run's
img:draw). Quantity parameters now go through lb_checkint/lb_optint: integers
pass through, floats round to nearest (ties toward +inf), and NaN, +/-inf and
floats beyond +/-2^24 (no longer exact integers in float32) raise a clean
argument error instead of wrapping.

The intarg_test fixture logs "IA <NAME> <value>" per probe, draws a fixed
scene on white, then logs "IA DONE"; the pixel probes below check that the
draws landed on the rounded pixel.
"""
import pytest


def _lines(sim):
    out = []
    for line in sim.get_log_buffer()["lines"]:
        out.append(line if isinstance(line, str) else line.get("text", ""))
    return out


def _run_fixture(sim, timeout=30.0):
    sim.clear_log()
    sim.launch_app("intarg_test")
    # The fixture logs "IA DONE" after its last probe and after the flush of
    # its scene, so the pixels are final once it appears.
    try:
        sim.wait_for_log(r"IA DONE", timeout=timeout)
    except TimeoutError:
        pytest.fail("IA DONE not seen within %.0fs:\n%s"
                    % (timeout, "\n".join(_lines(sim)[-40:])))
    results = {}
    for l in _lines(sim):
        idx = l.find("IA ")
        if idx < 0:
            continue
        name, _, value = l[idx + 3:].partition(" ")
        results[name] = value
    print("IA results:", results)
    return results


def _px(sim, x, y):
    return sim.call("get_pixel", {"x": x, "y": y})["rgb565"]


WHITE, BLACK = 0xFFFF, 0x0000
OUT_OF_RANGE = "number out of integer range"
NOT_FINITE = "number is NaN or infinite"


def test_float_quantity_arguments_round(simulator):
    r = _run_fixture(simulator)

    # Size: image.new(10.5, 4.4) -> 11 x 4
    assert r["SIZE"] == "11x4", r["SIZE"]
    # Duration: animator.new(99.99999 ms) sampled at 50.5 ms -> 51 / 100
    # (tostring prints "51.0" on both targets since the float formatter fix;
    # the numeric compare is kept as the looser check)
    assert float(r["DURATION"]) == 51.0, r["DURATION"]
    assert r["SLEEP"] == "ok", r["SLEEP"]
    assert r["REPEAT"] == "ok", r["REPEAT"]
    # Volume: 99.99999 -> 100, 10.5 -> 11 (ties up), -0.4 -> 0
    assert r["VOL_SET"] == "ok", r["VOL_SET"]
    assert r["VOL_A"] == "100"
    assert r["VOL_B"] == "11"
    assert r["VOL_C"] == "0"
    # Numeric strings still convert (luaL_checkinteger accepted them too)
    assert r["COORD_NUMSTR"] == "ok", r["COORD_NUMSTR"]
    # An exact integer is not range-limited (it is clipped by the draw)
    assert r["COORD_INT"] == "ok", r["COORD_INT"]


def test_unrepresentable_arguments_error_cleanly(simulator):
    r = _run_fixture(simulator)

    for name in ("SIZE_BIG", "DURATION_BIG", "VOL_BIG", "COORD_BIG"):
        assert r[name].startswith("err ") and OUT_OF_RANGE in r[name], (
            "%s: 1e9 must be rejected, got %r" % (name, r[name]))
    for name in ("COORD_NAN", "COORD_INF", "COORD_NEGINF"):
        assert r[name].startswith("err ") and NOT_FINITE in r[name], (
            "%s: got %r" % (name, r[name]))
    # Errors name the argument like any luaL_argerror (#1: Lua skips self).
    assert "bad argument #1 to 'draw'" in r["COORD_BIG"], r["COORD_BIG"]
    # Non-numbers keep the standard type error.
    assert "number expected" in r["COORD_STR"], r["COORD_STR"]


def test_float_coordinates_land_on_rounded_pixel(simulator):
    _run_fixture(simulator)

    # a = 11x4 black image drawn at (99.99999, 50) -> covers x 100..110, y 50..53
    assert _px(simulator, 99, 51) == WHITE
    assert _px(simulator, 100, 50) == BLACK
    assert _px(simulator, 110, 53) == BLACK
    assert _px(simulator, 111, 51) == WHITE
    assert _px(simulator, 105, 54) == WHITE

    # b = 2x2 black image drawn at (10.5, 70.5) -> covers x 11..12, y 71..72
    assert _px(simulator, 10, 71) == WHITE
    assert _px(simulator, 11, 70) == WHITE
    assert _px(simulator, 11, 71) == BLACK
    assert _px(simulator, 12, 72) == BLACK
    assert _px(simulator, 13, 72) == WHITE

    # b drawn at (-0.4, 90) -> covers x 0..1, y 90..91
    assert _px(simulator, 0, 90) == BLACK
    assert _px(simulator, 1, 91) == BLACK
    assert _px(simulator, 2, 90) == WHITE


# ── Task 30: narrow sinks, table-field errors, one rounding rule ──────────

def test_narrow_sinks_clamp_instead_of_wrapping(simulator):
    r = _run_fixture(simulator)

    # sampleplayer volume is a uint8_t: 300 used to wrap to 44, -1 to 255
    # (which the driver then capped at 100). Now 300 -> 255 -> capped 100.
    assert r["SVOL_300"] == "100", r["SVOL_300"]
    assert r["SVOL_NEG"] == "0", r["SVOL_NEG"]
    # rgb clamps each component before packing: 256 used to lose red
    # entirely ((256 & 0xF8) == 0), -3 / 300 would bleed across channels.
    assert r["RGB_HI"] == str(0xF800), r["RGB_HI"]
    assert r["RGB_NEG"] == str(0x07E0), r["RGB_NEG"]
    # valueAtTime's time is unsigned: -1.5 used to wrap to ~4e9 (end value)
    assert float(r["VAT_NEG"]) == 0.0, r["VAT_NEG"]


def test_table_field_errors_name_the_argument(simulator):
    r = _run_fixture(simulator)

    # Used to read "bad argument #-1 ..." with no hint which field was bad.
    assert "bad argument #1" in r["FIELD_ERR"], r["FIELD_ERR"]
    assert "field 'y1'" in r["FIELD_ERR"] and NOT_FINITE in r["FIELD_ERR"], r["FIELD_ERR"]
    assert "bad argument #1" in r["ENTRY_ERR"], r["ENTRY_ERR"]
    assert "sample: " + NOT_FINITE in r["ENTRY_ERR"], r["ENTRY_ERR"]
    # Numeric strings go through a float: "20000000" (> 2^24) is out of range
    assert OUT_OF_RANGE in r["NUMSTR_BIG"], r["NUMSTR_BIG"]


def test_display_primitives_round_like_everything_else(simulator):
    r = _run_fixture(simulator)

    # NaN / inf used to go through an undefined (int) cast; now an error
    for name, fn in (("FILL_NAN", "fillRect"), ("TEXT_INF", "drawText")):
        assert r[name].startswith("err ") and NOT_FINITE in r[name], (name, r[name])
        assert "bad argument #1 to '%s'" % fn in r[name], (name, r[name])

    # fillRect(10.5, ...) used to truncate to x = 10 while img:draw(10.5, ...)
    # rounded to 11; both land on x = 11 now.
    assert _px(simulator, 10, 110) == WHITE
    assert _px(simulator, 11, 110) == BLACK
    assert _px(simulator, 12, 110) == WHITE
    assert _px(simulator, 10, 114) == WHITE
    assert _px(simulator, 11, 114) == BLACK


def test_negative_ties_round_toward_positive_infinity(simulator):
    _run_fixture(simulator)

    # b (2x2) at x = -1.5 -> -1: covers x -1..0
    assert _px(simulator, 0, 100) == BLACK
    assert _px(simulator, 1, 100) == WHITE
    # b at x = -0.5 -> 0: covers x 0..1
    assert _px(simulator, 0, 104) == BLACK
    assert _px(simulator, 1, 105) == BLACK
    assert _px(simulator, 2, 104) == WHITE
