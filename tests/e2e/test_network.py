"""Tests for WiFi state management and network API.

Tests WiFi state injection/querying via RPC and basic HTTP connection
lifecycle via Lua fixture app. All tests run offline — no real network needed.

Regression targets:
- WiFi state machine correctness
- HTTP connection create/close lifecycle (no resource leaks)
- Graceful error handling when WiFi is disconnected
"""

import time
import pytest


class TestWifiStateManagement:
    """Test WiFi state reporting and injection via RPC."""

    def test_wifi_state_structure(self, simulator):
        """Test that get_wifi_state returns expected fields."""
        result = simulator.call("get_wifi_state")
        assert "status" in result
        assert "ssid" in result
        assert "ip" in result

    def test_wifi_state_valid_status(self, simulator):
        """Test that WiFi status is a valid value."""
        result = simulator.call("get_wifi_state")
        valid_statuses = {"online", "connected", "connecting", "disconnected"}
        assert result["status"] in valid_statuses, (
            f"Invalid WiFi status: {result['status']}"
        )

    def test_set_wifi_disconnected(self, simulator):
        """Test setting WiFi state to disconnected."""
        result = simulator.call("set_wifi_state", {"status": "disconnected"})
        assert result.get("ok"), f"set_wifi_state failed: {result}"

        state = simulator.call("get_wifi_state")
        assert state["status"] == "disconnected"

    def test_set_wifi_connected(self, simulator):
        """Test setting WiFi state to connected."""
        result = simulator.call("set_wifi_state", {"status": "connected"})
        assert result.get("ok"), f"set_wifi_state failed: {result}"

        state = simulator.call("get_wifi_state")
        assert state["status"] in ("connected", "online"), (
            f"Expected connected/online, got {state['status']}"
        )

    def test_wifi_state_roundtrip(self, simulator):
        """Test disconnect then reconnect cycle."""
        # Disconnect
        simulator.call("set_wifi_state", {"status": "disconnected"})
        state = simulator.call("get_wifi_state")
        assert state["status"] == "disconnected"

        # Reconnect
        simulator.call("set_wifi_state", {"status": "connected"})
        state = simulator.call("get_wifi_state")
        assert state["status"] != "disconnected"


class TestNetworkLua:
    """Test network operations via the network_test Lua app."""

    def test_network_operations(self, simulator):
        """Run the network_test app and verify all operations pass."""
        simulator.clear_log()
        simulator.launch_app("network_test")

        try:
            simulator.wait_for_log("NETWORK_TESTS_DONE", timeout=15)
        except TimeoutError:
            logs = simulator.get_log_buffer()
            pytest.fail(f"network_test did not complete in time. Logs: {logs}")

        logs = simulator.get_log_buffer()
        lines = [
            (l if isinstance(l, str) else l.get("text", ""))
            for l in logs.get("lines", [])
        ]

        results = [l for l in lines if l.startswith("PASS:") or l.startswith("FAIL:")]

        failures = [r for r in results if r.startswith("FAIL:")]
        assert not failures, f"Network test failures: {failures}"

        # At minimum, getStatus should pass
        passed = {r.split(":")[1] for r in results if r.startswith("PASS:")}
        assert "getStatus" in passed, f"getStatus should pass. Got: {results}"

    def test_network_no_heap_leak(self, simulator):
        """Test that HTTP connection lifecycle doesn't leak memory."""
        heap_before = simulator.call("get_heap_info")
        free_before = heap_before.get("lua_heap_free_kb", 0)

        simulator.clear_log()
        simulator.launch_app("network_test")

        try:
            simulator.wait_for_log("NETWORK_TESTS_DONE", timeout=15)
        except TimeoutError:
            pass

        time.sleep(1)

        heap_after = simulator.call("get_heap_info")
        free_after = heap_after.get("lua_heap_free_kb", 0)

        if free_before > 0:
            leak = free_before - free_after
            assert leak < 50, (
                f"Possible heap leak after network test: {leak}KB "
                f"(before={free_before}KB, after={free_after}KB)"
            )
