"""E2E tests for dev commands sent through the control channel.

The `dev_command` RPC queues one dev-command line for Core 0, exactly as a
line typed on the device's USB serial console: the launcher loop or a running
app's Lua hook / sys.sleep pump executes it. That is the path `push_app`
relies on on hardware (`unzip <zip> <dest>`, then `rm <zip>`).

The simulator cannot model the 4 KB main stack, so the unzip tests are
coverage for the command path and its reply; the stack fix itself (commands
run on a PSRAM app stack) is verified on hardware.
"""
import io
import time
import zipfile
from pathlib import Path

import pytest

FIXTURE_APP = Path(__file__).parent / "apps" / "zip_test"


def _dev(sim, cmd, timeout=20.0):
    return sim.call("dev_command", {"cmd": cmd}, timeout=timeout)


def _build_archive(dest: Path) -> dict:
    """A multi-file archive in push_app's shape: the zip_test fixture app plus
    nested directories and a binary member. Returns {name: bytes}."""
    members = {}
    for p in sorted(FIXTURE_APP.rglob("*")):
        if p.is_file():
            members[p.relative_to(FIXTURE_APP).as_posix()] = p.read_bytes()
    members["assets/img/tile.bin"] = bytes(range(256)) * 64
    members["assets/snd/a/b/deep.txt"] = b"deep nested\n" * 50
    members["empty.txt"] = b""
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members.items():
            z.writestr(name, data)
    return members


def _assert_extracted(root: Path, members: dict):
    for name, data in members.items():
        f = root / name
        assert f.is_file(), f"{name} was not extracted"
        assert f.read_bytes() == data, f"{name} differs after extraction"


def _wait_running(sim, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if sim.call("get_running_app").get("name"):
            return
        time.sleep(0.05)
    pytest.fail("app did not start")


def test_exit_at_launcher_is_refused(simulator):
    """`exit` with no app running is an error reply, not a shutdown/fault.

    On hardware the launcher loop used to return from launcher_run(), main()
    returned and newlib's _exit hit a breakpoint: a HardFault and reboot.
    """
    r = _dev(simulator, "exit")
    assert r["ok"] is False
    assert "no app running" in r["output"]

    r2 = simulator.exit_app()
    assert r2.get("ok") is False
    assert "no app running" in r2.get("message", "")

    time.sleep(0.5)
    assert simulator.is_alive(), "simulator shut down on exit at the launcher"
    assert simulator.ping()

    # The refused exit must not linger and kill the next app on launch.
    simulator.launch_app("harness_ok")
    outcome = simulator.wait_for_exit(timeout=15)
    assert outcome["result"] == "returned", outcome
    assert "H:DONE" in "\n".join(
        l if isinstance(l, str) else l.get("text", "")
        for l in simulator.get_log_buffer()["lines"])


def test_unzip_and_rm_at_launcher(simulator, test_sd_card):
    members = _build_archive(test_sd_card / "data" / "push.zip")

    r = _dev(simulator, "unzip /data/push.zip /data/unz")
    assert r["ok"] is True, r
    assert f"Unzipped {len(members)} files (0 skipped)" in r["output"]
    _assert_extracted(test_sd_card / "data" / "unz", members)

    r = _dev(simulator, "rm /data/unz")
    assert r["ok"] is True, r
    assert not (test_sd_card / "data" / "unz").exists()

    r = _dev(simulator, "unzip /data/missing.zip /data/unz")
    assert r["ok"] is False
    assert "Error: unzip failed" in r["output"]
    assert simulator.is_alive()


def test_unzip_while_lua_app_running(simulator, test_sd_card):
    """The command runs inside the app's Lua pump (on its app stack on
    hardware: no nested app-stack switch), then `exit` stops the app."""
    members = _build_archive(test_sd_card / "data" / "push.zip")

    simulator.launch_app("harness_ticker")
    _wait_running(simulator)

    r = _dev(simulator, "unzip /data/push.zip /data/unz_app")
    assert r["ok"] is True, r
    assert f"Unzipped {len(members)} files" in r["output"]
    _assert_extracted(test_sd_card / "data" / "unz_app", members)
    assert simulator.call("get_running_app").get("name"), \
        "app stopped during unzip"

    r = _dev(simulator, "exit")
    assert r["ok"] is True, r
    outcome = simulator.wait_for_exit(timeout=15)
    assert outcome["result"] in ("returned", "exit_sentinel"), outcome
    assert simulator.is_alive()
