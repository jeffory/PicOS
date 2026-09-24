"""game.save name handling and isolation (audit §3.2).

Hostile names must not reach outside the save area, and one app must not
read another's slots. Today saves live in a shared /saves and names are
spliced into the path unchecked; Task 6 validates names and moves saves to
/data/<app_id>/saves, which will turn the strict xfails below into passes.

A traversal that would leave the SD root entirely is refused by the
simulator's SD containment (Task 3), so no case can write to the host.
"""

import json

import pytest

from helpers import case_params, known_bug, lua_case_names, run_lua_app

SAVE_CASES = lua_case_names("save_test")

SAVE_BUG = ("review: Lua Critical — game.save builds /saves/%s.json from the "
            "raw name (.. resolves; long names truncate); Task 6")
SHARED_BUG = ("review: Lua Critical — all apps share /saves; Task 6 makes saves "
              "per-app")

SAVE_KNOWN_BUGS = {
    "traversal_set_rejected": SAVE_BUG,
    "traversal_exists_rejected": SAVE_BUG,
    "traversal_get_rejected": SAVE_BUG,
    "traversal_delete_rejected": SAVE_BUG,
    "long_names_do_not_collide": SAVE_BUG,
}

SYSTEM_CONFIG = {"sentinel": "keep"}


def _stage(sd):
    (sd / "system" / "config.json").write_text(json.dumps(SYSTEM_CONFIG))
    (sd / "saves").mkdir(exist_ok=True)
    (sd / "saves" / "bad.json").write_text('{"a":')


@pytest.fixture(scope="module")
def save_runs(sim_module_factory):
    sim = sim_module_factory(setup=_stage)
    run = run_lua_app(sim, "save_test", timeout=20)
    peer = run_lua_app(sim, "save_peer", timeout=20)
    return run, peer


@pytest.mark.parametrize("case", case_params(SAVE_CASES, SAVE_KNOWN_BUGS))
def test_game_save(save_runs, case):
    save_runs[0].check_case(case)


def test_game_save_suite_complete(save_runs):
    run, _ = save_runs
    run.assert_clean_exit()
    assert sorted(run.cases) == sorted(SAVE_CASES), run.describe()


def test_no_write_outside_sd_root(save_runs):
    run, _ = save_runs
    assert not (run.sd.parent / "escape.json").exists()
    assert not list(run.sd.parent.glob("*.json"))


@known_bug(SAVE_BUG)
def test_system_files_untouched(save_runs):
    run, _ = save_runs
    sysdir = run.sd / "system"
    assert not (sysdir / "pwn.json").exists(), "set('../system/pwn') wrote /system/pwn.json"
    cfg = sysdir / "config.json"
    assert cfg.exists(), "delete('../system/config') deleted /system/config.json"
    assert json.loads(cfg.read_text()) == SYSTEM_CONFIG


@known_bug(SHARED_BUG)
def test_other_app_cannot_read_slot(save_runs):
    _, peer = save_runs
    peer.assert_clean_exit()
    peer.check_case("isolation_peer_cannot_read")
