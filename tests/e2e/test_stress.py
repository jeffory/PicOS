"""Stress tests for resource management and stability under load.

These tests run longer than normal E2E tests and verify the OS handles
repeated operations without leaking resources or crashing.

Run with: pytest tests/e2e/test_stress.py -v
"""

import base64
import time
import pytest


@pytest.mark.slow
class TestRapidAppCycles:
    """Test rapid app launch/exit cycles for resource leaks."""

    def test_10_launch_exit_cycles(self, simulator):
        """Run 10 rapid app launch/exit cycles checking for leaks and crashes."""
        heap_initial = simulator.call("get_heap_info")
        free_initial = heap_initial.get("lua_heap_free_kb", 0)

        for i in range(10):
            simulator.clear_log()
            simulator.launch_app("sys_test")
            try:
                simulator.wait_for_log("SYS_TESTS_DONE", timeout=15)
            except TimeoutError:
                pytest.fail(f"App hung on cycle {i+1}/10")

            time.sleep(0.3)

            # Verify no crash after each cycle
            crash = simulator.call("get_crash_log")
            assert not crash.get("crash_log"), (
                f"Crash detected on cycle {i+1}: {crash['crash_log']}"
            )

        # Check for cumulative heap leak
        heap_final = simulator.call("get_heap_info")
        free_final = heap_final.get("lua_heap_free_kb", 0)

        if free_initial > 0:
            leak = free_initial - free_final
            assert leak < 200, (
                f"Heap leak after 10 cycles: {leak}KB "
                f"(initial={free_initial}KB, final={free_final}KB)"
            )

    def test_mixed_app_types(self, simulator):
        """Alternate between different fixture apps to detect cross-contamination."""
        apps = ["sys_test", "display_test", "fs_test"]
        sentinels = {
            "sys_test": "SYS_TESTS_DONE",
            "display_test": "DISPLAY_TESTS_DONE",
            "fs_test": "FS_TESTS_DONE",
        }

        for cycle in range(3):
            for app_name in apps:
                simulator.clear_log()
                simulator.launch_app(app_name)
                try:
                    simulator.wait_for_log(sentinels[app_name], timeout=15)
                except TimeoutError:
                    pytest.fail(
                        f"App '{app_name}' hung on cycle {cycle+1}"
                    )
                time.sleep(0.3)

        # Simulator should still be responsive
        result = simulator.call("ping")
        assert "uptime_ms" in result


@pytest.mark.slow
class TestFilesystemStress:
    """Test filesystem under write-heavy load."""

    def test_write_storm_50_files(self, simulator):
        """Write 50 small files rapidly, read them all back, then delete all."""
        paths = []
        test_data = b"stress_test_data_payload_1234567890"
        encoded = base64.b64encode(test_data).decode()

        # Write 50 files
        for i in range(50):
            path = f"/data/stress_{i:03d}.bin"
            result = simulator.call("write_file", {
                "path": path,
                "data": encoded,
            })
            assert result.get("ok"), f"Write failed for {path}: {result}"
            paths.append(path)

        # Read them all back and verify
        for path in paths:
            result = simulator.call("read_file", {"path": path})
            got = base64.b64decode(result["data"])
            assert got == test_data, f"Data mismatch in {path}"

        # Delete all
        for path in paths:
            simulator.call("delete_file", {"path": path})

        # Verify all deleted
        result = simulator.call("list_dir", {"path": "/data"})
        names = [e["name"] for e in result.get("entries", [])]
        for i in range(50):
            assert f"stress_{i:03d}.bin" not in names, (
                f"File stress_{i:03d}.bin should be deleted"
            )

    def test_overwrite_same_file_many_times(self, simulator):
        """Overwrite the same file 20 times with different content."""
        path = "/data/overwrite_stress.bin"

        for i in range(20):
            content = f"iteration_{i:03d}_data".encode() * 10
            encoded = base64.b64encode(content).decode()
            result = simulator.call("write_file", {
                "path": path,
                "data": encoded,
            })
            assert result.get("ok"), f"Write failed on iteration {i}"

        # Final read should match last write
        expected = f"iteration_019_data".encode() * 10
        result = simulator.call("read_file", {"path": path})
        got = base64.b64decode(result["data"])
        assert got == expected, "Final content doesn't match last write"

        simulator.call("delete_file", {"path": path})
