"""The harness's own safety nets (audit R2, R3, R7, C7, §5.0 goldens).

- a simulator that dies during a test fails that test, with the crash log
- a sanitizer report on stderr is detected (for real under an ASan build)
- a failing test gets the simulator's diagnostics attached
- a skip that is not on skip_allowlist.txt fails the run
- a failing @pytest.mark.flaky test is quarantined, not retried
- a missing golden image fails unless --update-baselines
- wait_for_exit recovers an app.exited notification the client never got
- a call whose reply can never come (connection closed) fails at once
- the Lua test kit reports PASS/FAIL/SKIP, survives identity tampering and
  writes test_results.json

The first four run an inner pytest session (pytester) that loads this suite's
conftest.py, so they test the real hooks.
"""

import re
import site
import socket
import textwrap
import threading
import time
from pathlib import Path

import numpy as np
import pytest

from helpers import E2E_DIR, compare_golden, run_lua_app, stage_lua_app
from picos_simulator import PicosSimulator

pytest_plugins = ["pytester"]

# Inner-session conftest: load the suite's conftest.py under another module
# name and re-export its fixtures and hooks.
INNER_CONFTEST = textwrap.dedent(f"""
    import importlib.util, sys
    sys.path.insert(0, {str(E2E_DIR)!r})
    _spec = importlib.util.spec_from_file_location(
        "picos_e2e_conftest", {str(E2E_DIR / "conftest.py")!r})
    _mod = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_mod)
    globals().update({{k: v for k, v in vars(_mod).items()
                      if not k.startswith("__")}})
""")


# Evidence of a SIGSEGV: the release sim's crash handler writes
# "Signal: SIGSEGV" to the crash log; a sanitizer build leaves the signal to
# ASan, which prints "AddressSanitizer: SEGV" on stderr instead.
SEGV_EVIDENCE = re.compile(r"Signal: SIGSEGV|AddressSanitizer: SEGV")


def _segv_count(out: str) -> int:
    return len(SEGV_EVIDENCE.findall(out))


def _inner(pytester, simulator_binary, test_src, *args):
    # pytester points HOME at a tmp dir, which hides a --user pytest install
    # from the subprocess; keep the real user site-packages visible.
    pytester._monkeypatch.setenv("PYTHONUSERBASE", site.getuserbase())
    pytester.makeconftest(INNER_CONFTEST)
    pytester.makepyfile(test_inner=textwrap.dedent(test_src))
    return pytester.runpytest_subprocess(
        "-p", "no:cacheprovider", "-p", "no:xdist",
        f"--simulator-path={simulator_binary}", *args, timeout=120)


def test_sim_crash_fails_the_test(pytester, simulator_binary):
    """A simulator killed by SIGSEGV mid-test fails the test and the report
    carries the crash handler's backtrace (read from disk)."""
    result = _inner(pytester, simulator_binary, """
        import os, signal, time
        def test_dies(simulator):
            os.kill(simulator.process.pid, signal.SIGSEGV)
            deadline = time.time() + 5
            while simulator.process.poll() is None and time.time() < deadline:
                time.sleep(0.05)
    """)
    result.assert_outcomes(failed=1)
    out = result.stdout.str()
    assert "Simulator health check failed" in out, out
    assert _segv_count(out), out           # crash log from disk (or ASan)
    assert "sim stderr (tail)" in out, out         # diagnostics attached


def test_failure_attaches_diagnostics(pytester, simulator_binary, tmp_path):
    """A failing test gets the log buffer, stdout/stderr tails and a
    screenshot; --artifacts-dir writes them to disk."""
    art = tmp_path / "artifacts"
    result = _inner(pytester, simulator_binary, """
        def test_fails(simulator):
            simulator.launch_app("harness_ok")
            simulator.wait_for_exit(timeout=10)
            assert False, "deliberate"
    """, f"--artifacts-dir={art}")
    result.assert_outcomes(failed=1)
    out = result.stdout.str()
    assert "sim log buffer" in out and "H:LINE 3" in out, out
    shots = list(art.rglob("screenshot.png"))
    assert shots and shots[0].read_bytes()[:4] == b"\x89PNG", list(art.rglob("*"))
    assert list(art.rglob("log_buffer.txt")), list(art.rglob("*"))


def test_unlisted_skip_fails_the_run(pytester, simulator_binary):
    result = _inner(pytester, simulator_binary, """
        import pytest
        def test_skips():
            pytest.skip("nobody allow-listed this")
    """)
    assert result.ret == pytest.ExitCode.TESTS_FAILED, result.stdout.str()
    result.stdout.fnmatch_lines(["*skips not on tests/e2e/skip_allowlist.txt*",
                                 "*test_skips*nobody allow-listed this*"])


def test_hardware_marked_skip_is_allowed(pytester, simulator_binary):
    result = _inner(pytester, simulator_binary, """
        import pytest
        @pytest.mark.hardware
        def test_needs_device():
            pytest.skip("no device")
    """)
    assert result.ret == pytest.ExitCode.OK, result.stdout.str()


def test_flaky_failure_is_quarantined_not_retried(pytester, simulator_binary):
    """A failing @pytest.mark.flaky test does not fail the run; it is listed
    in its own summary section and runs exactly once."""
    result = _inner(pytester, simulator_binary, """
        import pytest
        RUNS = []
        @pytest.mark.flaky(reason="known flake")
        def test_flake():
            RUNS.append(1)
            assert len(RUNS) > 1, "fails on first run"
    """, "-rx")
    assert result.ret == pytest.ExitCode.OK, result.stdout.str()
    result.assert_outcomes(xfailed=1)
    result.stdout.fnmatch_lines(["*quarantined flaky tests that failed*",
                                 "*test_flake (call): quarantined flake: known flake*"])


def test_flaky_health_failure_is_not_quarantined(pytester, simulator_binary):
    """A @pytest.mark.flaky test whose simulator crashed FAILS the run: the
    quarantine forgives flaky assertions, never crashes."""
    result = _inner(pytester, simulator_binary, """
        import os, signal, time, pytest
        @pytest.mark.flaky(reason="known flake")
        def test_flake_that_crashes(simulator):
            os.kill(simulator.process.pid, signal.SIGSEGV)
            deadline = time.time() + 5
            while simulator.process.poll() is None and time.time() < deadline:
                time.sleep(0.05)
    """)
    assert result.ret == pytest.ExitCode.TESTS_FAILED, result.stdout.str()
    result.assert_outcomes(failed=1)
    out = result.stdout.str()
    assert _segv_count(out), out
    assert "quarantined flaky tests that failed" not in out, out


CRASHER_APP = (
    'local T = picocalc.sys.loadlib("picotest")\n'
    'T.case("before", function() end)\n'
    'T.case("crash", function() picocalc.sys.triggerFault() end)\n'
    'T.case("after", function() end)\n'
    'T.done()\n')


def test_crash_mid_lua_suite_fails_every_case_with_evidence(pytester, simulator_binary):
    """A simulator that dies during a lua_suite run fails every case id, and
    each failure carries the crash log and stderr, not a socket error."""
    result = _inner(pytester, simulator_binary, f"""
        import pytest
        from helpers import stage_lua_app

        APP = {CRASHER_APP!r}

        @pytest.fixture(scope="module")
        def run(lua_suite):
            return lua_suite("crasher", setup=lambda sd: stage_lua_app(sd, "crasher", APP))

        @pytest.mark.parametrize("case", ["before", "crash", "after"])
        def test_case(run, case):
            run.check_case(case)
    """)
    result.assert_outcomes(failed=3)
    out = result.stdout.str()
    # Each failure is the crash report, not an escaped socket error.
    assert "E       RuntimeError" not in out, out
    assert out.count("E       AssertionError: app crasher: outcome") == 3, out
    assert _segv_count(out) >= 3, out         # crash log (or ASan) per case
    assert out.count("stderr tail:") >= 3, out            # stderr per case
    assert out.count("simulator_died") >= 3, out


def test_call_fails_at_once_when_the_connection_closes_mid_call():
    """The simulator closes the connection after reading a request (it
    crashed or exited): the pending call fails with a connection error at
    once, not a TimeoutError after the full RPC timeout. run_lua_app tells a
    dead simulator from a hung one by that difference; an ASan build that
    died mid-call used to be reported as "timeout"."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)

    def serve():
        conn, _ = srv.accept()
        conn.recv(4096)  # the request; no reply
        conn.close()

    threading.Thread(target=serve, daemon=True).start()
    sim = PicosSimulator(timeout=10.0)
    sim.tcp_port = srv.getsockname()[1]
    sim._connect()
    try:
        start = time.monotonic()
        with pytest.raises(RuntimeError):
            sim.call("get_last_outcome")
        assert time.monotonic() - start < 2.0
    finally:
        sim._disconnect()
        srv.close()


def test_sim_factory_sims_are_health_checked(pytester, simulator_binary):
    """Simulators started through sim_factory (a function, not a simulator
    fixture value) are still health-checked after the call."""
    result = _inner(pytester, simulator_binary, """
        import os, signal, time
        from helpers import build_sd_card
        def test_dies(sim_factory, tmp_path):
            sim = sim_factory(build_sd_card(tmp_path / "sd"))
            os.kill(sim.process.pid, signal.SIGSEGV)
            deadline = time.time() + 5
            while sim.process.poll() is None and time.time() < deadline:
                time.sleep(0.05)
    """)
    result.assert_outcomes(failed=1)
    out = result.stdout.str()
    assert "Simulator health check failed" in out and _segv_count(out), out


def test_sanitizer_report_is_detected(simulator):
    """health_problems() flags a sanitizer line on the simulator's stderr."""
    assert simulator.health_problems() == []
    simulator._stderr_tail.append(
        "==4242==ERROR: AddressSanitizer: heap-use-after-free on address 0x1")
    problems = simulator.health_problems()
    simulator._stderr_tail.pop()  # don't fail this test's own health check
    assert any("AddressSanitizer" in p for p in problems), problems


def test_sanitizer_report_survives_later_output(simulator):
    """The stderr drain keeps a sanitizer report even after more output than
    the 2000-line tail holds has followed it (a TSan run keeps printing;
    C-Dogs floods the tails), and it stays bounded."""
    note = simulator._note_stderr_line
    note("==4242==ERROR: AddressSanitizer: heap-use-after-free on address 0x1")
    note("    #0 0x1 in l_fs_write lua_bridge_fs.c:123")
    for i in range(5000):
        note(f"noise {i}")
    try:
        assert "#0 0x1 in l_fs_write" in simulator.sanitizer_report()
        assert len(simulator._sanitizer_report) == simulator.SANITIZER_REPORT_MAX
        assert any("AddressSanitizer" in p for p in simulator.health_problems())
    finally:
        simulator._sanitizer_report.clear()  # don't fail this test's own check


@pytest.mark.asan_only
def test_real_sanitizer_report_fails_the_test(pytester, simulator_binary):
    """End to end, through the real stderr drain: an ASan build's
    sanitizer_selftest RPC reads past a heap block, ASan prints its report
    and aborts, and the health hook fails the test with the report and its
    stack."""
    result = _inner(pytester, simulator_binary, """
        import time
        def test_overflow(simulator):
            try:
                simulator.call("sanitizer_selftest", timeout=5)
            except Exception:
                pass  # the sim is dead
            deadline = time.time() + 10
            while simulator.process.poll() is None and time.time() < deadline:
                time.sleep(0.05)
    """)
    result.assert_outcomes(failed=1)
    out = result.stdout.str()
    assert "Simulator health check failed" in out, out
    assert "AddressSanitizer: heap-buffer-overflow" in out, out
    assert "h_sanitizer_selftest" in out, out  # the stack came through


def test_sanitizer_expectation_mismatch_fails_the_run(pytester, simulator_binary):
    """PICOS_SIM_EXPECT_SANITIZE names a sanitizer the binary's --build-info
    does not report: the run stops with a usage error instead of running
    (and skipping every asan_only test)."""
    pytester._monkeypatch.setenv("PICOS_SIM_EXPECT_SANITIZE", "memory")
    pytester._monkeypatch.delenv("PICOS_SIM_SANITIZE", raising=False)
    result = _inner(pytester, simulator_binary, """
        def test_never_runs():
            pass
    """)
    assert result.ret == pytest.ExitCode.USAGE_ERROR, result.stdout.str()
    err = result.stderr.str()
    assert "PICOS_SIM_EXPECT_SANITIZE=memory" in err and "missing memory" in err, err


def test_sanitizer_expectation_with_failed_probe_fails_the_run(pytester, tmp_path):
    """An expected sanitizer build whose --build-info probe fails (here: no
    binary) is a usage error, not a silent release run."""
    pytester._monkeypatch.setenv("PICOS_SIM_EXPECT_SANITIZE", "address")
    pytester._monkeypatch.delenv("PICOS_SIM_SANITIZE", raising=False)
    result = _inner(pytester, tmp_path / "no_such_simulator", """
        def test_never_runs():
            pass
    """)
    assert result.ret == pytest.ExitCode.USAGE_ERROR, result.stdout.str()
    assert "--build-info` failed" in result.stderr.str(), result.stderr.str()


def test_asan_only_skip_not_allowed_when_probe_fails(pytester, tmp_path):
    """asan_only skips are allow-listed only when the probe says the binary
    is a release build; if the probe failed the skip fails the run."""
    pytester._monkeypatch.delenv("PICOS_SIM_EXPECT_SANITIZE", raising=False)
    pytester._monkeypatch.delenv("PICOS_SIM_SANITIZE", raising=False)
    result = _inner(pytester, tmp_path / "no_such_simulator", """
        import pytest
        @pytest.mark.asan_only
        def test_needs_asan():
            pass
    """)
    result.assert_outcomes(skipped=1)
    assert result.ret == pytest.ExitCode.TESTS_FAILED, result.stdout.str()
    assert "skips not on tests/e2e/skip_allowlist.txt" in result.stdout.str()


def test_sanitizer_options_merge_per_key(monkeypatch):
    """A caller's ASAN_OPTIONS overrides only the keys it sets; the suite's
    other options survive (setdefault used to drop them all)."""
    from picos_simulator import sanitizer_env
    env = sanitizer_env({"ASAN_OPTIONS": "detect_leaks=1:verbosity=1"})
    opts = dict(o.split("=", 1) for o in env["ASAN_OPTIONS"].split(":"))
    assert opts == {"abort_on_error": "1", "halt_on_error": "1",
                    "detect_leaks": "1", "verbosity": "1"}, opts
    assert "allocator_may_return_null" not in env["ASAN_OPTIONS"]


def test_missing_golden_fails(tmp_path):
    img = np.zeros((4, 4, 3), dtype=np.uint8)
    missing = tmp_path / "nope.png"
    with pytest.raises(pytest.fail.Exception, match="golden image missing"):
        compare_golden(img, missing, update=False)
    assert not missing.exists(), "a missing golden must not be written"
    compare_golden(img, missing, update=True)       # --update-baselines
    compare_golden(img, missing, update=False)      # now it matches
    img[0, 0] = 255
    with pytest.raises(AssertionError, match="differs in 1 pixels"):
        compare_golden(img, missing, update=False)


def test_wait_for_exit_recovers_a_lost_notification(simulator):
    """app.exited can be dropped whole while the client's buffer is full;
    wait_for_exit then gets the outcome from get_last_outcome."""
    assert simulator.get_last_outcome() == {"launch_id": 0}
    launch = simulator.launch_app("harness_ok")
    assert launch["launch_id"] >= 1, launch
    simulator.wait_for_log(r"^\[LAUNCHER\] exited harness_ok$", timeout=10, src="os")
    # Simulate the drop: remove the notification the client received.
    with simulator._notif_cond:
        simulator._notifications[:] = [
            n for n in simulator._notifications if n.get("method") != "app.exited"]
    outcome = simulator.wait_for_exit(timeout=3)
    assert outcome["name"] == "harness_ok", outcome
    assert outcome["result"] == "returned", outcome
    assert outcome["launch_id"] == launch["launch_id"], outcome
    assert simulator.get_last_outcome() == outcome


def test_unix_socket_disabled_and_tcp_on_loopback(simulator):
    """--unix-socket none leaves no picos_control in the cwd; TCP listens on
    127.0.0.1 only."""
    out = simulator.get_output()["stdout"]
    assert "UNIX domain server disabled" in out, out[-2000:]
    assert "UNIX domain server listening" not in out
    assert not list(Path.cwd().glob("picos_control*"))
    listening = Path("/proc/net/tcp").read_text().splitlines()[1:]
    port_hex = f"{simulator.tcp_port:04X}"
    ours = [l.split() for l in listening if l.split()[1].endswith(":" + port_hex)
            and l.split()[3] == "0A"]  # 0A = LISTEN
    assert ours, f"port {simulator.tcp_port} not listening"
    assert all(cols[1].startswith("0100007F:") for cols in ours), ours


# ── Lua test kit ────────────────────────────────────────────────────────────

KIT_APP = """
local T = picocalc.sys.loadlib("picotest")
T.case("passes", function() T.eq(1 + 1, 2) end)
T.case("fails_eq", function() T.eq(1, 2, "one vs two") end)
T.case("errors", function() local x = nil; return x.field end)
T.case("skips", function() T.skip("not here") end)
T.case("raises_ok", function() T.raises(function() error("boom") end, "boom") end)
T.case("raises_none", function() T.raises(function() end) end)
T.case("tamper", function()
    APP_ID = "com.other"
    APP_REQUIREMENTS.root_filesystem = true
    APP_REQUIREMENTS = { root_filesystem = true }
end)
T.case("identity_restored", function()
    T.eq(APP_ID, "com.test.kit_selftest")
    T.eq(APP_REQUIREMENTS.root_filesystem, false)
end)
T.done()
"""


def test_picotest_kit_reports_every_outcome(simulator):
    stage_lua_app(simulator.sd_card_path, "kit_selftest", KIT_APP)
    run = run_lua_app(simulator, "kit_selftest", timeout=15)
    run.assert_clean_exit()
    status = {n: c["status"] for n, c in run.cases.items()}
    assert status == {
        "passes": "PASS", "fails_eq": "FAIL", "errors": "FAIL", "skips": "SKIP",
        "raises_ok": "PASS", "raises_none": "FAIL", "tamper": "PASS",
        "identity_restored": "PASS",
    }, run.describe()
    assert "one vs two: expected 2, got 1" in run.cases["fails_eq"]["detail"]
    assert ":4: one vs two" in run.cases["fails_eq"]["detail"]  # check position
    assert run.cases["skips"]["detail"] == "not here"
    res = run.results
    assert res and res["done"] is True, res
    assert (res["pass"], res["fail"], res["skip"]) == (4, 3, 1), res
    texts = [e["text"] for e in run.log]
    assert any(t.startswith("[T] CASE fails_eq FAIL ") and
               t.endswith(":4: one vs two: expected 2, got 1") for t in texts), texts
    assert "[T] DONE pass=4 fail=3 skip=1" in texts


def test_picotest_results_survive_a_crash_mid_suite(simulator):
    """Results are rewritten after every case, so cases that finished before
    the app died are still reported (here: a Lua error outside any case)."""
    stage_lua_app(simulator.sd_card_path, "kit_partial", """
local T = picocalc.sys.loadlib("picotest")
T.case("first", function() end)
error("app dies between cases")
""")
    run = run_lua_app(simulator, "kit_partial", timeout=15)
    assert run.outcome["result"] == "error", run.describe()
    assert run.cases == {"first": {"status": "PASS", "detail": ""}}, run.describe()
    assert not run.done
    with pytest.raises(AssertionError):
        run.assert_clean_exit()
