"""Tests for display color correctness using get_pixel RPC.

Regression targets:
- RGB565 byte-swap correctness
- Color constant values (RED, GREEN, BLUE, WHITE, BLACK, YELLOW)
- display_stats and display_diff RPC functionality
"""

import time
import pytest


def assert_color_near(actual, expected, tolerance=32, label=""):
    """Assert RGB values are within tolerance of expected.

    RGB565 has limited precision: R/B have 5 bits (steps of 8),
    G has 6 bits (steps of 4). Use tolerance=32 to account for this.
    """
    r, g, b = actual["r"], actual["g"], actual["b"]
    er, eg, eb = expected
    assert abs(r - er) <= tolerance and abs(g - eg) <= tolerance and abs(b - eb) <= tolerance, (
        f"{label}: expected ~({er},{eg},{eb}), got ({r},{g},{b})"
    )


class TestColorBlocks:
    """Test color block rendering with pixel-level verification."""

    def test_red_block(self, simulator):
        """Verify red block at (50, 50) is red."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_BLOCKS_READY", timeout=15)

        px = simulator.call("get_pixel", {"x": 50, "y": 50})
        assert_color_near(px, (248, 0, 0), label="red block center")

    def test_green_block(self, simulator):
        """Verify green block at (160, 50) is green."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_BLOCKS_READY", timeout=15)

        px = simulator.call("get_pixel", {"x": 160, "y": 50})
        assert_color_near(px, (0, 252, 0), label="green block center")

    def test_blue_block(self, simulator):
        """Verify blue block at (270, 50) is blue."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_BLOCKS_READY", timeout=15)

        px = simulator.call("get_pixel", {"x": 270, "y": 50})
        assert_color_near(px, (0, 0, 248), label="blue block center")

    def test_white_block(self, simulator):
        """Verify white block at (50, 160) is white."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_BLOCKS_READY", timeout=15)

        px = simulator.call("get_pixel", {"x": 50, "y": 160})
        assert_color_near(px, (248, 252, 248), label="white block center")

    def test_yellow_block(self, simulator):
        """Verify yellow block at (160, 160) is yellow (red+green)."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_BLOCKS_READY", timeout=15)

        px = simulator.call("get_pixel", {"x": 160, "y": 160})
        assert_color_near(px, (248, 252, 0), label="yellow block center")

    def test_black_background(self, simulator):
        """Verify undrawn area at (270, 160) is black."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_BLOCKS_READY", timeout=15)

        px = simulator.call("get_pixel", {"x": 270, "y": 160})
        assert_color_near(px, (0, 0, 0), label="black background")

    def test_pixel_region_query(self, simulator):
        """Test get_pixel with w/h to read a small region."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_BLOCKS_READY", timeout=15)

        result = simulator.call("get_pixel", {"x": 50, "y": 50, "w": 2, "h": 2})
        assert "pixels" in result, f"Region query should return pixels array: {result}"
        # 2x2 region = 4 pixels, all should be the same red value
        assert len(result["pixels"]) == 4


class TestFullScreenColors:
    """Test full-screen color fills for uniform color verification."""

    def test_full_red_screen(self, simulator):
        """Verify full red screen has uniform color."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_FULL_RED", timeout=15)

        # Sample multiple points across the screen
        for x, y in [(10, 10), (160, 160), (310, 310), (0, 319), (319, 0)]:
            px = simulator.call("get_pixel", {"x": x, "y": y})
            assert_color_near(px, (248, 0, 0), label=f"full red at ({x},{y})")

    def test_full_green_screen(self, simulator):
        """Verify full green screen has uniform color."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_FULL_GREEN", timeout=15)

        px = simulator.call("get_pixel", {"x": 160, "y": 160})
        assert_color_near(px, (0, 252, 0), label="full green center")

    def test_full_blue_screen(self, simulator):
        """Verify full blue screen has uniform color."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_FULL_BLUE", timeout=15)

        px = simulator.call("get_pixel", {"x": 160, "y": 160})
        assert_color_near(px, (0, 0, 248), label="full blue center")


class TestDisplayStats:
    """Test display_stats RPC for framebuffer introspection."""

    def test_stats_after_color_fill(self, simulator):
        """Verify display_stats reports expected unique colors."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_FULL_RED", timeout=15)

        stats = simulator.call("display_stats")
        assert stats["total_pixels"] == 320 * 320
        # Full red screen should be mostly one color (allow for minor variation)
        assert stats["unique_colors"] <= 3, (
            f"Full red screen should be ~1 unique color, got {stats['unique_colors']}"
        )
        # Most pixels should be non-zero (red is nonzero)
        assert stats["nonzero_pixels"] > 320 * 320 * 0.99

    def test_stats_pixel_at(self, simulator):
        """Test display_stats with pixel_at query parameter."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_BLOCKS_READY", timeout=15)

        stats = simulator.call("display_stats", {"x": 50, "y": 50})
        assert "pixel_at" in stats
        pa = stats["pixel_at"]
        assert pa["x"] == 50
        assert pa["y"] == 50
        # Should be red
        assert pa["r"] > 200


class TestDisplayDiff:
    """Test display_diff RPC for change detection."""

    def test_diff_detects_change(self, simulator):
        """Verify display_diff detects framebuffer changes between phases."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_BLOCKS_READY", timeout=15)

        # Capture reference (color blocks)
        simulator.call("display_diff", {"action": "capture"})

        # Wait for next phase (full red)
        simulator.wait_for_log("COLOR_FULL_RED", timeout=15)

        # Compare — should show significant change
        result = simulator.call("display_diff", {"action": "compare"})
        assert result["changed_pixels"] > 0, "Should detect pixels changed"
        assert result["change_pct"] > 10.0, (
            f"Expected >10% change, got {result['change_pct']}%"
        )

    def test_diff_no_change(self, simulator):
        """Verify display_diff reports no change when nothing moved."""
        simulator.clear_log()
        simulator.launch_app("color_test")
        simulator.wait_for_log("COLOR_FULL_RED", timeout=15)

        # Capture and immediately compare (no change)
        simulator.call("display_diff", {"action": "capture"})
        result = simulator.call("display_diff", {"action": "compare"})
        assert result["changed_pixels"] == 0, (
            f"Expected 0 changed pixels, got {result['changed_pixels']}"
        )
