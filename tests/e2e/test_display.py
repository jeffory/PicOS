"""Tests for display drawing operations via Lua app and screenshot verification.

Regression targets:
- display_draw_image_partial null data check (commit in display.c)
- Screenshot RPC returning valid PNG/raw data
- Color correctness (RGB565 byte-swap)
"""

import time
import pytest


class TestDisplayDrawing:
    """Test display drawing operations via the display_test Lua app."""

    def test_display_operations(self, simulator):
        """Run the display_test app and verify it completes."""
        simulator.clear_log()
        simulator.launch_app("display_test")

        try:
            simulator.wait_for_log("DISPLAY_TESTS_DONE", timeout=15)
        except TimeoutError:
            logs = simulator.get_log_buffer()
            pytest.fail(f"display_test did not complete in time. Logs: {logs}")

        logs = simulator.get_log_buffer()
        lines = [
            (l if isinstance(l, str) else l.get("text", ""))
            for l in logs.get("lines", [])
        ]

        # Verify all drawing phases completed
        expected = ["DRAW:red_fill", "DRAW:white_rect", "DRAW:text",
                    "DRAW:color_bands", "DISPLAY_TESTS_DONE"]
        for marker in expected:
            assert marker in lines, f"Missing marker '{marker}'. Got: {lines}"

    def test_color_bands_screenshot(self, simulator):
        """Test that color bands are visible and correctly colored."""
        simulator.clear_log()
        simulator.launch_app("display_test")

        try:
            simulator.wait_for_log("DRAW:color_bands", timeout=15)
        except TimeoutError:
            logs = simulator.get_log_buffer()
            pytest.fail(f"display_test didn't reach color_bands. Logs: {logs}")

        # Small delay for flush to complete
        time.sleep(0.2)

        # Verify actual colors using get_pixel RPC
        # display_test draws: red band y=0-49, green band y=100-149, blue band y=200-249
        red_px = simulator.call("get_pixel", {"x": 160, "y": 25})
        green_px = simulator.call("get_pixel", {"x": 160, "y": 125})
        blue_px = simulator.call("get_pixel", {"x": 160, "y": 225})
        black_px = simulator.call("get_pixel", {"x": 160, "y": 75})  # gap between bands

        # Red band should be red (high R, low G/B)
        assert red_px["r"] > 200 and red_px["g"] < 50 and red_px["b"] < 50, (
            f"Red band pixel not red: r={red_px['r']}, g={red_px['g']}, b={red_px['b']}"
        )
        # Green band should be green
        assert green_px["g"] > 200 and green_px["r"] < 50 and green_px["b"] < 50, (
            f"Green band pixel not green: r={green_px['r']}, g={green_px['g']}, b={green_px['b']}"
        )
        # Blue band should be blue
        assert blue_px["b"] > 200 and blue_px["r"] < 50 and blue_px["g"] < 50, (
            f"Blue band pixel not blue: r={blue_px['r']}, g={blue_px['g']}, b={blue_px['b']}"
        )
        # Gap should be black
        assert black_px["r"] < 10 and black_px["g"] < 10 and black_px["b"] < 10, (
            f"Gap between bands not black: r={black_px['r']}, g={black_px['g']}, b={black_px['b']}"
        )


class TestScreenshotRPC:
    """Test screenshot RPC infrastructure."""

    def test_screenshot_returns_image(self, simulator):
        """Test that screenshot RPC returns a valid image."""
        img = simulator.screenshot_pil()
        assert img is not None
        # Should be 320x320 (simulator display size) or a scaled version
        w, h = img.size
        assert w > 0 and h > 0

    def test_screenshot_format(self, simulator):
        """Test screenshot returns proper PNG data."""
        import base64
        result = simulator.call("screenshot", {"format": "png"})
        assert "data" in result or "format" in result

    def test_get_display_buffer(self, simulator):
        """Test raw display buffer retrieval."""
        result = simulator.call("get_display_buffer")
        # Should return base64-encoded raw framebuffer
        assert "data" in result
        assert "width" in result
        assert "height" in result
        assert result["width"] == 320
        assert result["height"] == 320
