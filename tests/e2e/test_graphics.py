"""E2E tests for picocalc.graphics (tilemap drawing).

Drives the graphics_test fixture and asserts on its "GT" log markers plus
get_pixel probes of the drawn tilemap.
"""
import pytest

# RGB888 as the sim reports it for the RGB565 tile colours.
RED, GREEN, BLUE, YELLOW = (248, 0, 0), (0, 252, 0), (0, 0, 248), (248, 252, 0)


def _assert_near(px, expected, label, tolerance=32):
    got = (px["r"], px["g"], px["b"])
    assert all(abs(a - b) <= tolerance for a, b in zip(got, expected)), (
        f"{label}: expected ~{expected}, got {got}")


def _lines(sim):
    return [l if isinstance(l, str) else l.get("text", "")
            for l in sim.get_log_buffer()["lines"]]


@pytest.fixture
def graphics_app(simulator):
    simulator.clear_log()
    simulator.launch_app("graphics_test")
    simulator.wait_for_log("GT DONE", timeout=20)
    yield simulator
    simulator.keypress("esc")


def test_tilemap_draws_every_tile_from_its_source_rect(graphics_app):
    """A 2x2 tilemap in reversed tile order must show all four tileset
    tiles, each at its map cell (tiles 2-4 have non-zero source offsets)."""
    joined = "\n".join(_lines(graphics_app))
    assert "GT TILEMAP_READY" in joined, joined
    # Map cell (col,row) -> tile index: (0,0)=4, (1,0)=3, (0,1)=2, (1,1)=1.
    probes = [((16, 16), YELLOW, "cell 0,0 = tile 4"),
              ((48, 16), BLUE,   "cell 1,0 = tile 3"),
              ((16, 48), GREEN,  "cell 0,1 = tile 2"),
              ((48, 48), RED,    "cell 1,1 = tile 1")]
    for (x, y), colour, label in probes:
        _assert_near(graphics_app.call("get_pixel", {"x": x, "y": y}),
                     colour, label)

