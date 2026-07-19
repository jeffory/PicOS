"""Tests for filesystem operations via Lua app and RPC.

Regression targets:
- SD card flush on close/mkdir/delete/rename (commit 0965131)
- Multi-block write reliability (commit 6143878)
- File sandbox enforcement
- Large file write/read roundtrip (exercises multi-block SD path)
"""

import base64
import time
import pytest


class TestFilesystemLua:
    """Test filesystem operations via the fs_test Lua app."""

    def test_fs_operations(self, simulator):
        """Run the fs_test app and verify all FS operations pass."""
        simulator.clear_log()
        simulator.launch_app("fs_test")

        # Wait for the app to finish (it exits after all tests)
        try:
            simulator.wait_for_log("FS_TESTS_DONE", timeout=15)
        except TimeoutError:
            logs = simulator.get_log_buffer()
            pytest.fail(f"fs_test did not complete in time. Logs: {logs}")

        # Collect results
        logs = simulator.get_log_buffer()
        lines = [
            (l if isinstance(l, str) else l.get("text", ""))
            for l in logs.get("lines", [])
        ]

        # Find all PASS/FAIL lines
        results = [l for l in lines if l.startswith("PASS:") or l.startswith("FAIL:")]

        expected_tests = [
            "write_read", "readFile", "exists", "size", "seek_tell",
            "mkdir", "delete", "rename", "listDir",
            "large_write", "large_read", "diskInfo",
        ]

        failures = [r for r in results if r.startswith("FAIL:")]
        assert not failures, f"FS test failures: {failures}"

        # Verify we got results for all expected tests
        passed = {r.split(":")[1] for r in results if r.startswith("PASS:")}
        for name in expected_tests:
            assert name in passed, f"Missing result for test '{name}'. Got: {results}"


class TestFilesystemRPC:
    """Test filesystem operations via direct RPC calls."""

    def test_write_read_roundtrip(self, simulator):
        """Test file write/read roundtrip via RPC."""
        test_data = b"RPC write test: Hello from E2E!\x00\xff\x01"
        encoded = base64.b64encode(test_data).decode()

        result = simulator.call("write_file", {
            "path": "/data/rpc_test.bin",
            "data": encoded,
        })
        assert result.get("ok"), f"write_file failed: {result}"
        assert result.get("bytes_written") == len(test_data)

        # Read it back
        result = simulator.call("read_file", {"path": "/data/rpc_test.bin"})
        assert result.get("size") == len(test_data)
        got = base64.b64decode(result["data"])
        assert got == test_data, f"Data mismatch: {got!r} != {test_data!r}"

        # Clean up
        simulator.call("delete_file", {"path": "/data/rpc_test.bin"})

    def test_large_file_roundtrip(self, simulator):
        """Test 32KB file write/read (exercises multi-block writes).

        Regression test for SD card multi-block write reliability (commit 6143878).
        """
        # 32KB pattern — fits within the 256KB base64 decode buffer
        pattern = bytes(range(256)) * 128  # 32KB
        encoded = base64.b64encode(pattern).decode()

        result = simulator.call("write_file", {
            "path": "/data/rpc_large.bin",
            "data": encoded,
        }, timeout=30)
        assert result.get("ok"), f"write_file failed: {result}"

        result = simulator.call("read_file", {"path": "/data/rpc_large.bin"})
        got = base64.b64decode(result["data"])
        assert len(got) == len(pattern), f"Size mismatch: {len(got)} != {len(pattern)}"
        assert got == pattern, "Large file data corruption detected"

        simulator.call("delete_file", {"path": "/data/rpc_large.bin"})

    def test_list_dir(self, simulator):
        """Test directory listing via RPC."""
        result = simulator.call("list_dir", {"path": "/apps"})
        entries = result.get("entries", [])
        assert len(entries) > 0, "No entries in /apps"

        names = [e["name"] for e in entries]
        assert "hello" in names, f"hello app not found in {names}"

        for entry in entries:
            assert "name" in entry
            assert "is_dir" in entry

    def test_disk_info(self, simulator):
        """Test disk space reporting."""
        result = simulator.call("disk_info")
        assert "free_kb" in result
        assert "total_kb" in result
        assert result["total_kb"] > 0
        assert result["free_kb"] >= 0
        assert result["free_kb"] <= result["total_kb"]

    def test_file_create_and_delete(self, simulator):
        """Test file creation and deletion cycle."""
        simulator.call("write_file", {
            "path": "/data/delete_me.txt",
            "data": base64.b64encode(b"temp").decode(),
        })

        # Verify exists
        result = simulator.call("list_dir", {"path": "/data"})
        names = [e["name"] for e in result.get("entries", [])]
        assert "delete_me.txt" in names

        # Delete
        simulator.call("delete_file", {"path": "/data/delete_me.txt"})

        # Verify gone
        result = simulator.call("list_dir", {"path": "/data"})
        names = [e["name"] for e in result.get("entries", [])]
        assert "delete_me.txt" not in names

    def test_overwrite_file(self, simulator):
        """Test that writing to an existing file overwrites it."""
        path = "/data/overwrite_test.txt"

        # Write initial content
        simulator.call("write_file", {
            "path": path,
            "data": base64.b64encode(b"initial content here").decode(),
        })

        # Overwrite with shorter content
        simulator.call("write_file", {
            "path": path,
            "data": base64.b64encode(b"new").decode(),
        })

        result = simulator.call("read_file", {"path": path})
        got = base64.b64decode(result["data"])
        assert got == b"new", f"Expected b'new', got {got!r}"

        simulator.call("delete_file", {"path": path})
