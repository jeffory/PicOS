"""Stress tests for resource management and stability under load.

These tests run longer than normal E2E tests and verify the OS handles
repeated operations without leaking resources or crashing.

Run with: pytest tests/e2e/test_stress.py -v
"""

import base64

import pytest

from helpers import HEAP_METRICS_XFAIL, assert_heap_metrics_live, run_lua_app


@pytest.mark.slow
class TestRapidAppCycles:
    """Test rapid app launch/exit cycles for resource leaks."""

    def test_10_launch_exit_cycles(self, simulator):
        """10 launch/exit cycles: every run completes cleanly and the sim
        stays healthy (process alive, no crash log)."""
        for i in range(10):
            run = run_lua_app(simulator, "sys_test", timeout=15)
            assert run.outcome.get("result") == "returned", (
                f"cycle {i + 1}/10: {run.describe()}")
            assert run.done and not run.problems, f"cycle {i + 1}/10: {run.describe()}"

    @HEAP_METRICS_XFAIL
    def test_10_cycles_no_heap_leak(self, simulator):
        """10 cycles leave no more than 200 KB behind."""
        assert_heap_metrics_live(simulator)
        free_initial = simulator.call("get_heap_info")["lua_heap_free_kb"]
        for i in range(10):
            run_lua_app(simulator, "sys_test", timeout=15).assert_clean_exit()
        free_final = simulator.call("get_heap_info")["lua_heap_free_kb"]
        leak = free_initial - free_final
        assert leak < 200, (
            f"Heap leak after 10 cycles: {leak}KB "
            f"(initial={free_initial}KB, final={free_final}KB)")

    def test_mixed_app_types(self, simulator):
        """Alternate fixture apps in one sim; no run may report a failure."""
        for cycle in range(3):
            for app_name in ("sys_test", "fs_test"):
                run = run_lua_app(simulator, app_name, timeout=15)
                bad = {n: c for n, c in run.cases.items() if c["status"] != "PASS"}
                assert run.done and not bad, (
                    f"{app_name} cycle {cycle + 1}: {run.describe()}")
            simulator.launch_app("display_test")
            # (display_test holds its last frame for 3 s before returning.
            # Don't shorten it with exit_app: that also injects Esc, which
            # quits the launcher if the app has already gone.)
            outcome = simulator.wait_for_exit(timeout=15)
            assert outcome.get("result") == "returned", (
                f"display_test cycle {cycle + 1}: {outcome}")


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
