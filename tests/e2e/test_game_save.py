"""game.save name handling, isolation and round-tripping (audit §3.2).

Hostile names must not reach outside the save area, and one app must not
read another's slots. Saves live in /data/<app_id>/saves/<name>.json and
names are limited to [A-Za-z0-9._-]; a save left at the pre-Task-6 shared
/saves/<name>.json is moved into the first app that asks for that name.

Names are refused at the bridge before any path is built; behind that, the
simulator's SD containment (Task 3) keeps any stray path on the SD image.
"""

import json

import pytest

from helpers import case_params, lua_case_names, run_lua_app

SAVE_CASES = lua_case_names("save_test")

SAVE_KNOWN_BUGS = {}

SYSTEM_CONFIG = {"sentinel": "keep"}
SAVE_DIR = ("data", "com.test.save", "saves")


def _stage(sd):
    (sd / "system" / "config.json").write_text(json.dumps(SYSTEM_CONFIG))
    saves = sd.joinpath(*SAVE_DIR)
    saves.mkdir(parents=True, exist_ok=True)
    (saves / "bad.json").write_text('{"a":')
    (sd / "saves").mkdir(exist_ok=True)
    (sd / "saves" / "legacy.json").write_text('{"high_score":695}')


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


def test_system_files_untouched(save_runs):
    run, _ = save_runs
    sysdir = run.sd / "system"
    assert not (sysdir / "pwn.json").exists(), "set('../system/pwn') wrote /system/pwn.json"
    cfg = sysdir / "config.json"
    assert cfg.exists(), "delete('../system/config') deleted /system/config.json"
    assert json.loads(cfg.read_text()) == SYSTEM_CONFIG


def test_other_app_cannot_read_slot(save_runs):
    _, peer = save_runs
    peer.assert_clean_exit()
    peer.check_case("isolation_peer_cannot_read")


def test_saves_land_in_app_data_dir(save_runs):
    run, _ = save_runs
    saves = run.sd.joinpath(*SAVE_DIR)
    for name in ("ok_slot", "slot1", "list_a"):
        f = saves / f"{name}.json"
        assert f.exists(), f"{f} missing: {sorted(p.name for p in saves.iterdir())}"
        assert not (run.sd / "saves" / f"{name}.json").exists()
    assert not (saves / "list_b.json").exists(), "delete left list_b.json"


def test_legacy_save_moved_into_app(save_runs):
    run, _ = save_runs
    moved = run.sd.joinpath(*SAVE_DIR) / "legacy.json"
    assert moved.exists(), "legacy save was not migrated"
    assert json.loads(moved.read_text()) == {"high_score": 695}
    assert not (run.sd / "saves" / "legacy.json").exists(), \
        "legacy save left behind in the shared /saves"
