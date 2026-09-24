"""Sandbox enforcement for Lua apps (audit §3.1).

sandbox_test has no requirements; sandbox_root_test has root-filesystem and
proves the same operations succeed with the grant. Each Lua case is one pytest
id; the host then checks that nothing was written outside the app's own data
directory.

Identity and grants are held in C (app_identity), so the cases that rewrite
the APP_* globals prove the globals are irrelevant to enforcement. Any future
known-bug case is a strict xfail listed in KNOWN_BUGS.
"""

import json

import pytest

from helpers import case_params, lua_case_names, write_mod, write_mp3, write_wav

SANDBOX_CASES = lua_case_names("sandbox_test")
ROOT_CASES = lua_case_names("sandbox_root_test")

KNOWN_BUGS = {}

SYSTEM_CONFIG = {"sentinel": "keep"}
OTHER_CONFIG = {"secret": "other-app"}


def _stage(sd):
    (sd / "system" / "config.json").write_text(json.dumps(SYSTEM_CONFIG))
    other = sd / "data" / "com.other"
    other.mkdir(parents=True, exist_ok=True)
    (other / "config.json").write_text(json.dumps(OTHER_CONFIG))
    write_wav(other / "secret.wav")
    write_mp3(other / "secret.mp3")
    write_mod(other / "secret.mod")
    own = sd / "apps" / "sandbox_test"
    write_wav(own / "own.wav")
    write_mp3(own / "own.mp3")
    write_mod(own / "own.mod")


@pytest.fixture(scope="module")
def sandbox_run(lua_suite):
    return lua_suite("sandbox_test", setup=_stage)


@pytest.fixture(scope="module")
def root_run(lua_suite):
    return lua_suite("sandbox_root_test", setup=_stage)


@pytest.mark.parametrize("case", case_params(SANDBOX_CASES, KNOWN_BUGS))
def test_sandbox(sandbox_run, case):
    sandbox_run.check_case(case)


# Host-side outcome of each case: files that must NOT exist afterwards.
FORBIDDEN_FILES = {
    "write_other_app_data_denied": ["data/com.other/baseline_x"],
    "globals_app_id": ["data/com.other/globals_x"],
    "prefix_confusion_denied": ["data/com.test.sandboxEVIL/x"],
    "read_system_lib": ["system/lib/evil.lua"],
    "relative_path_denied": ["x"],
    "sample_save_outside_sandbox": ["system/pwn.wav"],
}


@pytest.mark.parametrize("case", case_params(
    sorted(FORBIDDEN_FILES), {k: v for k, v in KNOWN_BUGS.items()
                              if k in FORBIDDEN_FILES}))
def test_sandbox_host_side(sandbox_run, case):
    """The denial held on disk, not just in the Lua return value."""
    written = [p for p in FORBIDDEN_FILES[case] if (sandbox_run.sd / p).exists()]
    assert not written, f"{case}: written outside the sandbox: {written}"


def test_system_config_untouched(sandbox_run):
    """No case may rewrite /system/config.json (appconfig with
    APP_ID=../system did)."""
    got = json.loads((sandbox_run.sd / "system" / "config.json").read_text())
    assert got == SYSTEM_CONFIG, f"/system/config.json was rewritten: {got}"


def test_sandbox_suite_complete(sandbox_run):
    sandbox_run.assert_clean_exit()
    assert sorted(sandbox_run.cases) == sorted(SANDBOX_CASES), sandbox_run.describe()


@pytest.mark.parametrize("case", ROOT_CASES)
def test_sandbox_root_control(root_run, case):
    root_run.check_case(case)


def test_sandbox_root_suite_complete(root_run):
    root_run.assert_all_passed(ROOT_CASES)
