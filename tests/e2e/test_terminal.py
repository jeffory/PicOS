"""Tests for terminal buffer dump via simulator RPC.

These tests launch a Lua app that creates a terminal, writes known text,
then verify the buffer contents match via get_terminal_buffer.
"""

import pytest


class TestTerminalBuffer:
    """Test terminal buffer dump functionality."""

    def test_terminal_buffer_returns_text(self, simulator):
        """Test that get_terminal_buffer returns written text."""
        simulator.clear_log()
        simulator.launch_app("term_test")
        simulator.wait_for_log("TERM_READY", timeout=10)

        buf = simulator.get_terminal_buffer()

        assert buf["cols"] == 53
        assert buf["rows"] == 26
        assert len(buf["lines"]) == 26

        # First line should contain "Hello, World!"
        assert buf["lines"][0] == "Hello, World!"
        # Second line should contain "Line 2 here"
        assert buf["lines"][1] == "Line 2 here"

    def test_terminal_cursor_position(self, simulator):
        """Test that cursor position is reported correctly."""
        simulator.clear_log()
        simulator.launch_app("term_test")
        simulator.wait_for_log("TERM_READY", timeout=10)

        buf = simulator.get_terminal_buffer()

        # After writing "Line 2 here" (no trailing newline), cursor at end of that text
        # "Line 2 here" is 11 chars, on row 1 (row 0 = "Hello, World!")
        assert buf["cursor_x"] == 11
        assert buf["cursor_y"] == 1

    def test_empty_lines_are_trimmed(self, simulator):
        """Test that trailing spaces on empty lines are trimmed."""
        simulator.clear_log()
        simulator.launch_app("term_test")
        simulator.wait_for_log("TERM_READY", timeout=10)

        buf = simulator.get_terminal_buffer()

        # Lines after the text should be empty strings (trimmed)
        for i in range(2, 26):
            assert buf["lines"][i] == "", f"Line {i} should be empty, got: '{buf['lines'][i]}'"

    def test_no_terminal_returns_error(self, simulator):
        """Test that querying without an active terminal gives an error."""
        # Don't launch any app — no terminal active
        with pytest.raises(RuntimeError, match="no active terminal"):
            simulator.get_terminal_buffer()
