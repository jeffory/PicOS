"""Tests for audio subsystem via Lua app and RPC.

Verifies tone generation, volume control, and audio state reporting.
Uses get_audio_state RPC to check playback status.
"""

import time
import pytest


class TestAudioLua:
    """Test audio operations via the audio_test Lua app."""

    def test_audio_operations(self, simulator):
        """Run the audio_test app and verify all audio operations pass."""
        simulator.clear_log()
        simulator.launch_app("audio_test")

        try:
            simulator.wait_for_log("AUDIO_TESTS_DONE", timeout=15)
        except TimeoutError:
            logs = simulator.get_log_buffer()
            pytest.fail(f"audio_test did not complete in time. Logs: {logs}")

        logs = simulator.get_log_buffer()
        lines = [
            (l if isinstance(l, str) else l.get("text", ""))
            for l in logs.get("lines", [])
        ]

        results = [l for l in lines if l.startswith("PASS:") or l.startswith("FAIL:")]

        failures = [r for r in results if r.startswith("FAIL:")]
        assert not failures, f"Audio test failures: {failures}"

        expected_tests = ["setVolume", "playTone", "stopTone", "volume_range"]
        passed = {r.split(":")[1] for r in results if r.startswith("PASS:")}
        for name in expected_tests:
            assert name in passed, f"Missing result for test '{name}'. Got: {results}"


class TestAudioRPC:
    """Test audio via direct RPC calls."""

    def test_play_tone_rpc(self, simulator):
        """Test play_tone RPC starts a tone."""
        result = simulator.call("play_tone", {
            "frequency": 440,
            "duration_ms": 2000,
        })
        assert result.get("ok"), f"play_tone failed: {result}"

        # Check audio state — tone should be playing
        time.sleep(0.1)
        state = simulator.call("get_audio_state")
        assert state["tone_playing"] is True, (
            f"Tone should be playing after play_tone. State: {state}"
        )

        # Stop it
        simulator.call("stop_audio")

    def test_stop_audio_rpc(self, simulator):
        """Test stop_audio RPC stops playback."""
        # Start a tone
        simulator.call("play_tone", {"frequency": 880, "duration_ms": 5000})
        time.sleep(0.1)

        # Stop it
        result = simulator.call("stop_audio")
        assert result.get("ok"), f"stop_audio failed: {result}"

        time.sleep(0.1)
        state = simulator.call("get_audio_state")
        assert state["tone_playing"] is False, (
            f"Tone should not be playing after stop_audio. State: {state}"
        )

    def test_audio_state_structure(self, simulator):
        """Test get_audio_state returns expected fields."""
        state = simulator.call("get_audio_state")
        assert "tone_playing" in state
        assert "stream_active" in state
        assert "sound_players_active" in state
        assert isinstance(state["tone_playing"], bool)
        assert isinstance(state["stream_active"], bool)
        assert isinstance(state["sound_players_active"], int)

    def test_idle_audio_state(self, simulator):
        """Test audio state when nothing is playing."""
        # Ensure nothing is playing
        simulator.call("stop_audio")
        time.sleep(0.1)

        state = simulator.call("get_audio_state")
        assert state["tone_playing"] is False
        assert state["stream_active"] is False
