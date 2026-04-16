"""Test basic launcher functionality."""

import time
import numpy as np
import pytest
from display import DisplayVerifier


def wait_for_launcher(simulator, timeout=5):
    """Wait until the launcher has rendered (non-uniform framebuffer)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        img = simulator.screenshot_pil()
        arr = np.array(img)[:, :, :3]
        if len(np.unique(arr.reshape(-1, 3), axis=0)) > 2:
            return img
        time.sleep(0.2)
    return simulator.screenshot_pil()


def wait_for_app_running(simulator, timeout=5):
    """Wait until an app is running (not the launcher)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            status = simulator.call("get_running_app")
            if status and status.get("name"):
                return status
        except Exception:
            pass
        time.sleep(0.2)
    return None


class TestLauncher:
    """Tests for PicOS launcher."""

    def test_launcher_starts(self, simulator):
        """Test that launcher initializes and shows available apps."""
        screenshot = wait_for_launcher(simulator)
        assert screenshot.size == (320, 320)

        arr = np.array(screenshot)[:, :, :3]
        unique_colors = len(np.unique(arr.reshape(-1, 3), axis=0))
        assert unique_colors > 1, \
            "Launcher should have drawn UI elements (screen is uniform color)"

    def test_list_apps(self, simulator):
        """Test that apps are discovered and listed."""
        wait_for_launcher(simulator)

        apps = simulator.list_apps()
        assert len(apps) > 0, "No apps found"

        app_names = [a.lower() for a in apps]
        assert any("hello" in name for name in app_names), \
            f"hello app not found in {apps}"

    def test_launch_app(self, simulator):
        """Test launching an app from the launcher."""
        wait_for_launcher(simulator)

        result = simulator.launch_app("hello")
        assert result, "Failed to launch app"

        status = wait_for_app_running(simulator)
        assert status is not None, "App did not start running"

        screenshot = simulator.screenshot_pil()
        assert screenshot.size == (320, 320)

    def test_navigate_launcher(self, simulator):
        """Test navigating the launcher menu with arrow keys."""
        wait_for_launcher(simulator, timeout=8)

        apps = simulator.list_apps()
        if len(apps) <= 1:
            pytest.skip("Need multiple apps to test navigation")

        # Take a snapshot before navigation
        simulator.call("display_diff", {"action": "capture"})

        # Navigate down
        simulator.keypress("down")
        time.sleep(0.3)

        # Check if display changed using display_diff
        result_after = simulator.call("display_diff", {"action": "compare"})
        changed_pixels = result_after.get("changed_pixels", 0)

        assert changed_pixels > 0, \
            "Navigation should change the display (selection highlight)"

    def test_launch_by_directory_name(self, simulator):
        """Regression test: launcher_launch_by_name matches directory names."""
        wait_for_launcher(simulator)

        # Launch by directory name (not app ID or display name)
        result = simulator.launch_app("hello")
        assert result, "Failed to launch app by directory name"

        status = wait_for_app_running(simulator)
        assert status is not None, "App should be running after launch by dir name"
