"""Lua bridge robustness (Task 31): REPL formatting and scrollback,
terminal.new bounds, create-the-userdata-first ordering, mp3player handle
lifetime and fileplayer callback slots. The cases live in
apps/bridge_robust/main.lua (picotest); one pytest id per case.
"""

import pytest

from helpers import lua_case_names

CASES = lua_case_names("bridge_robust")


@pytest.fixture(scope="module")
def robust_run(lua_suite):
    return lua_suite("bridge_robust")


@pytest.mark.parametrize("case", CASES)
def test_bridge_robust_case(robust_run, case):
    robust_run.check_case(case)


def test_bridge_robust_complete(robust_run):
    robust_run.assert_all_passed(CASES)
