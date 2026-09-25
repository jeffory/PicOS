"""Lua bridge robustness (Task 31): REPL formatting and scrollback,
terminal.new bounds, create-the-userdata-first ordering, mp3player handle
lifetime and fileplayer callback slots. The cases live in
apps/bridge_robust/main.lua (picotest); one pytest id per case.
"""

import pytest

from helpers import lua_case_names, write_mp3

APP = "bridge_robust"
CASES = lua_case_names(APP)


def _setup(sd):
    # ~52 s of silence: still playing when the mp3player cases check.
    write_mp3(sd / "apps" / APP / "long.mp3", frames=2000)


@pytest.fixture(scope="module")
def robust_run(lua_suite):
    return lua_suite(APP, setup=_setup, timeout=60)


@pytest.mark.parametrize("case", CASES)
def test_bridge_robust_case(robust_run, case):
    robust_run.check_case(case)


def test_bridge_robust_complete(robust_run):
    robust_run.assert_all_passed(CASES)
