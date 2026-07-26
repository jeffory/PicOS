"""E2E tests for the panels.lua interactive-comics framework.

Drives the text-only panels_test fixture comic through every state machine
transition — advance, choice branch, story-var payoff, save on exit, resume
on relaunch — asserting on the library's own "PANELS:" log markers plus the
fixture's "PT " markers. panels.lua is staged onto the test SD from
system/lib/ (the canonical copy) at test time, exactly as firmware ships it.
"""
import shutil
import time
from pathlib import Path

import pytest

# Injected buttons are held ~80ms; taps of the same key must be spaced past it.
BUTTON_HOLD_S = 0.13
# Advance mode plays a 400ms snap animation and drops input while it runs, so
# consecutive advance taps must clear it too.
ADVANCE_GAP_S = 0.55
# Sequence changes default to a fadeToBlack transition (450ms out + 450ms in);
# "PANELS:SEQ n" logs at the midpoint, so input works ~450ms after it appears.
FADE_SETTLE_S = 0.8

REPO_ROOT = Path(__file__).resolve().parents[2]


def _lines(sim):
    out = []
    for line in sim.get_log_buffer()["lines"]:
        out.append(line if isinstance(line, str) else line.get("text", ""))
    return out


def _wait_for(sim, marker, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        joined = "\n".join(_lines(sim))
        if marker in joined:
            return joined
        time.sleep(0.2)
    pytest.fail(f"marker {marker!r} not seen within {timeout}s:\n"
                + "\n".join(_lines(sim)[-30:]))


def _tap(sim, key, count=1):
    for _ in range(count):
        sim.keypress(key)
        time.sleep(BUTTON_HOLD_S)


def _stage_panels_lib(test_sd_card):
    src = REPO_ROOT / "system" / "lib" / "panels.lua"
    assert src.exists(), "system/lib/panels.lua missing from the repo"
    dest = test_sd_card / "system" / "lib"
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy(src, dest / "panels.lua")


def test_panels_full_lifecycle(simulator, test_sd_card):
    _stage_panels_lib(test_sd_card)

    simulator.launch_app("panels_test")
    joined = _wait_for(simulator, "PANELS:SEQ 1")
    assert "PT NOLIB" not in joined
    assert "PT START" in joined

    # Custom renderFunction ran with the panel clip active.
    _wait_for(simulator, "PT RENDERFN")

    # Advance mode: forward key steps panels, then crosses into sequence 2.
    _tap(simulator, "down")          # panel 1 -> panel 2 (400ms snap)
    time.sleep(ADVANCE_GAP_S)
    _tap(simulator, "down")          # past last panel -> sequence 2
    _wait_for(simulator, "PANELS:SEQ 2")
    time.sleep(FADE_SETTLE_S)        # let the fade-in finish and choice arm

    # Choice UI: select the SECOND option, confirm, land in sequence 3 with
    # the story var set by that choice.
    _tap(simulator, "down")          # highlight "second"
    _tap(simulator, "enter")
    joined = _wait_for(simulator, "PANELS:SEQ 3")
    assert "PANELS:CHOICE 2 -> seq 3" in joined
    _wait_for(simulator, "PT PAYOFF picked=b")

    # ESC saves progress and exits cleanly back to the launcher.
    _tap(simulator, "esc")
    joined = _wait_for(simulator, "PT EXIT quit")
    assert "PANELS:EXIT quit" in joined

    # Relaunch: resume=true restores the saved sequence, not sequence 1.
    simulator.clear_log()
    simulator.launch_app("panels_test")
    joined = _wait_for(simulator, "PANELS:SEQ 3")
    assert "PANELS:SEQ 1" not in joined, "resume went back to the start"

    _tap(simulator, "esc")
    _wait_for(simulator, "PT EXIT quit")
