"""Tests for WiFi state management and network API.

Tests WiFi state injection/querying via RPC and basic HTTP connection
lifecycle via Lua fixture app. All tests run offline — no real network needed.

Regression targets:
- WiFi state machine correctness
- HTTP connection create/close lifecycle (no resource leaks)
- Graceful error handling when WiFi is disconnected
"""

import pytest

from helpers import (HEAP_METRICS_XFAIL, assert_heap_metrics_live,
                     lua_case_names, run_lua_app)


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


NETWORK_CASES = lua_case_names("network_test")


@pytest.fixture(scope="module")
def network_run(lua_suite):
    return lua_suite("network_test")


class TestNetworkLua:
    """network_test (picotest kit): one pytest id per Lua case."""

    @pytest.mark.parametrize("case", NETWORK_CASES)
    def test_network_case(self, network_run, case):
        network_run.check_case(case)

    def test_network_suite_complete(self, network_run):
        network_run.assert_all_passed(NETWORK_CASES)

    @HEAP_METRICS_XFAIL
    def test_network_no_heap_leak(self, simulator):
        """HTTP connection create/close leaves no more than 50 KB behind."""
        assert_heap_metrics_live(simulator)
        free_before = simulator.call("get_heap_info")["lua_heap_free_kb"]
        run_lua_app(simulator, "network_test", timeout=15).assert_all_passed(
            NETWORK_CASES)
        free_after = simulator.call("get_heap_info")["lua_heap_free_kb"]
        leak = free_before - free_after
        assert leak < 50, (
            f"Possible heap leak after network test: {leak}KB "
            f"(before={free_before}KB, after={free_after}KB)")
