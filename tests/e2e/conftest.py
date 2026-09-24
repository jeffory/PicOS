"""pytest fixtures and hooks for the PicOS E2E suite.

Usage (from any directory):
    SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto
    pytest tests/e2e --show-window          # watch the simulator
    pytest tests/e2e --update-baselines     # rewrite golden PNGs

Every simulator a test uses is health-checked after the test (process alive,
no crash log, no sanitizer report on stderr); a failing test gets the
simulator's stdout/stderr tails, log buffer, /system/error.log and a
screenshot attached. Helpers (SD staging, the Lua test kit runner) live in
helpers.py.
"""

import base64
import fnmatch
import os
import re
import subprocess
from pathlib import Path

import pytest

from helpers import (DEFAULT_SD_SOURCE, E2E_DIR, build_sd_card, new_simulator,
                     run_lua_app, stop_and_check)
from picos_simulator import PicosSimulator, binary_sanitizers

SKIP_ALLOWLIST = E2E_DIR / "skip_allowlist.txt"


def pytest_addoption(parser):
    parser.addoption(
        "--update-baselines", action="store_true", default=False,
        help="Rewrite golden PNGs instead of comparing against them",
    )
    parser.addoption(
        "--show-window", action="store_true", default=False,
        help="Show the simulator's SDL window (default: headless)",
    )
    parser.addoption(
        "--port", action="store", type=int, default=0,
        help="TCP port for the simulator (0 = auto-assign)",
    )
    parser.addoption(
        "--simulator-path", action="store",
        default=str(PicosSimulator.DEFAULT_BINARY),
        help="Path to the picos_simulator binary (default: $PICOS_SIM_BINARY, "
             "else build_sim/picos_simulator)",
    )
    parser.addoption(
        "--sd-card-path", action="store", default=str(DEFAULT_SD_SOURCE),
        help="SD card the manifest takes apps/hello (and sd-marker extras) from",
    )
    parser.addoption(
        "--artifacts-dir", action="store", default=None,
        help="Write per-failure diagnostics (stdout/stderr, log, error.log, "
             "screenshot) under this directory",
    )


def sim_is_sanitized(config) -> bool:
    """True when the simulator under test is an ASan build: its
    `--build-info` says so (make simulator-asan), or PICOS_SIM_SANITIZE is
    set by hand."""
    if os.environ.get("PICOS_SIM_SANITIZE"):
        return "address" in os.environ["PICOS_SIM_SANITIZE"]
    binary = Path(config.getoption("--simulator-path"))
    return binary.exists() and "address" in binary_sanitizers(binary)


def pytest_collection_modifyitems(config, items):
    """asan_only tests need an ASan build of the simulator
    (PICOS_SIM_BINARY=build_sim_asan/picos_simulator); otherwise they skip."""
    if not any("asan_only" in item.keywords for item in items):
        return
    if sim_is_sanitized(config):
        return
    skip = pytest.mark.skip(reason="asan_only: needs an ASan build of the "
                                   "simulator (make simulator-asan; "
                                   "PICOS_SIM_BINARY=build_sim_asan/picos_simulator)")
    for item in items:
        if "asan_only" in item.keywords:
            item.add_marker(skip)


# ── Simulator lifecycle ─────────────────────────────────────────────────────


@pytest.fixture(scope="session")
def simulator_binary(request) -> Path:
    """The simulator binary, built on demand."""
    binary_path = Path(request.config.getoption("--simulator-path")).resolve()
    # Only the default release binary is built on demand; a missing
    # PICOS_SIM_BINARY / --simulator-path must not silently fall back to it.
    if not binary_path.exists() and binary_path == PicosSimulator.PROJECT_ROOT / "build_sim" / "picos_simulator":
        try:
            PicosSimulator.build()
        except subprocess.CalledProcessError as e:
            pytest.fail(f"Failed to build simulator: {e}")
    if not binary_path.exists():
        pytest.fail(f"Simulator binary not found: {binary_path}")
    return binary_path


@pytest.fixture
def test_sd_card(tmp_path, request):
    """Per-test SD card built from the manifest (see helpers.build_sd_card).
    `@pytest.mark.sd(extra=[...])` stages more."""
    marker = request.node.get_closest_marker("sd")
    extra = marker.kwargs.get("extra", ()) if marker else ()
    return build_sd_card(tmp_path / "sd_card", extra=extra,
                         default_sd=Path(request.config.getoption("--sd-card-path")))


@pytest.fixture
def sim_factory(request, simulator_binary, tmp_path):
    """Start extra simulators: sim_factory(sd_path, **PicosSimulator kwargs).
    Each is stopped and health-checked at teardown."""
    sims = []

    def start(sd_path, **kwargs):
        crash = tmp_path / f"crash_{len(sims)}.log"
        sim = new_simulator(request.config, simulator_binary, sd_path, crash,
                            **kwargs)
        sims.append(sim)
        _register(request.node, sim)
        return sim

    yield start
    errors = []
    for sim in sims:
        if getattr(sim, "_health_reported", False):
            sim.stop()  # the test already failed on this sim's health
            continue
        try:
            stop_and_check(sim)
        except pytest.fail.Exception as e:
            errors.append(str(e))
    if errors:
        pytest.fail("\n".join(errors), pytrace=False)


@pytest.fixture
def simulator(sim_factory, test_sd_card) -> PicosSimulator:
    """A fresh simulator per test on the per-test SD card."""
    return sim_factory(test_sd_card)


@pytest.fixture(scope="module")
def sim_module_factory(request, simulator_binary, tmp_path_factory):
    """Module-scoped simulators: start(setup=None, **kwargs) boots one on a
    fresh manifest SD card (`setup(sd_path)` runs before boot). For modules
    that share one run across many test ids. Stopped and health-checked when
    the module finishes."""
    sims = []

    def start(setup=None, **kwargs):
        base = tmp_path_factory.mktemp("modsim")
        sd = build_sd_card(base / "sd_card",
                           default_sd=Path(request.config.getoption("--sd-card-path")))
        if setup:
            setup(sd)
        sim = new_simulator(request.config, simulator_binary, sd,
                            base / "crash.log", **kwargs)
        sims.append(sim)
        _register(request.node, sim)
        return sim

    yield start
    errors = []
    for sim in sims:
        if getattr(sim, "_health_reported", False):
            sim.stop()
            continue
        try:
            stop_and_check(sim)
        except pytest.fail.Exception as e:
            errors.append(str(e))
    if errors:
        pytest.fail("\n".join(errors), pytrace=False)


@pytest.fixture(scope="module")
def sim_module(sim_module_factory):
    """One simulator shared by a module (manifest SD card)."""
    return sim_module_factory()


@pytest.fixture(scope="session")
def lua_suite(request, simulator_binary, tmp_path_factory):
    """Run a picotest fixture app once in its own simulator:
    lua_suite(name, setup=None, timeout=30) -> helpers.LuaRun.

    For module-scoped fixtures that expand an app's cases into one pytest id
    each. `setup(sd_path)` runs before boot (stage files). The simulator is
    stopped after the run; its health problems land in LuaRun.problems.
    """
    def run(name, setup=None, timeout=30.0):
        base = tmp_path_factory.mktemp(f"suite_{name}")
        sd = build_sd_card(base / "sd_card",
                           default_sd=Path(request.config.getoption("--sd-card-path")))
        if setup:
            setup(sd)
        sim = new_simulator(request.config, simulator_binary, sd,
                            base / "crash.log")
        try:
            result = run_lua_app(sim, name, timeout=timeout)
        finally:
            sim.stop()
        result.problems.extend(p for p in sim.health_problems()
                               if p not in result.problems)
        return result

    return run


@pytest.fixture
def update_baselines(request):
    return request.config.getoption("--update-baselines")


# ── Health check and failure diagnostics (audit R2) ─────────────────────────


def _register(node, sim):
    """Record a factory-made simulator on a collection node (the test item
    for sim_factory, the module for sim_module_factory) so the health hook
    finds it; factories hand tests a function, not the simulator."""
    if not hasattr(node, "_picos_sims"):
        node._picos_sims = []
    node._picos_sims.append(sim)


def _sims_of(item) -> list:
    """Every PicosSimulator the test used: fixture values that are
    simulators, plus simulators registered by sim_factory on the item and
    by sim_module_factory on its module. (lua_suite stops its simulator
    before returning; its health lands in LuaRun.problems instead.)"""
    funcargs = getattr(item, "funcargs", {}) or {}
    found = [v for v in funcargs.values() if isinstance(v, PicosSimulator)]
    for node in item.listchain():
        found += getattr(node, "_picos_sims", [])
    seen, out = set(), []
    for sim in found:
        if id(sim) not in seen:
            seen.add(id(sim))
            out.append(sim)
    return out


def _tail(text: str, n: int = 200) -> str:
    lines = text.splitlines()
    return "\n".join(lines[-n:])


def _collect_diagnostics(sim: PicosSimulator) -> dict:
    """Text sections plus an optional screenshot (PNG bytes)."""
    diag = {}
    out = sim.get_output()
    diag["stdout (tail)"] = _tail(out["stdout"])
    diag["stderr (tail)"] = _tail(out["stderr"])
    crash = sim.read_crash_log()
    if crash:
        diag["crash log"] = crash
    error_log = Path(sim.sd_card_path) / "system" / "error.log"
    if error_log.exists():
        diag["/system/error.log"] = _tail(error_log.read_text(errors="replace"))
    png = None
    if sim.is_alive():
        try:
            lines = sim.get_log_lines(0)
            diag["log buffer"] = "\n".join(
                f"{e.get('seq')} [{e.get('src')}] {e.get('text')}" for e in lines)
        except Exception as e:  # diagnostics are best effort
            diag["log buffer"] = f"(unavailable: {e})"
        try:
            png = sim.screenshot()
        except Exception as e:
            diag["screenshot"] = f"(unavailable: {e})"
    return {"sections": diag, "png": png}


def _attach(item, report, sims):
    html = item.config.pluginmanager.getplugin("html")
    extras = getattr(report, "extras", [])
    art_root = item.config.getoption("--artifacts-dir")
    for i, sim in enumerate(sims):
        d = _collect_diagnostics(sim)
        label = f"sim{i}" if len(sims) > 1 else "sim"
        for name, text in d["sections"].items():
            report.sections.append((f"{label} {name}", text))
            if html:
                extras.append(html.extras.text(text, name=f"{label} {name}"))
        if d["png"] and html:
            extras.append(html.extras.png(base64.b64encode(d["png"]).decode(),
                                          name=f"{label} screenshot"))
        if art_root:
            safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", item.nodeid)
            out = Path(art_root) / safe / label
            out.mkdir(parents=True, exist_ok=True)
            for name, text in d["sections"].items():
                fname = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_") + ".txt"
                (out / fname).write_text(text)
            if d["png"]:
                (out / "screenshot.png").write_bytes(d["png"])
    if html:
        report.extras = extras


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    if report.when not in ("setup", "call"):
        return
    sims = _sims_of(item)
    if not sims:
        _quarantine(item, report)
        return
    if report.when == "call":
        problems = []
        for sim in sims:
            found = sim.health_problems()
            if found:
                # Reported here (or expected by an xfail): don't fail the
                # fixture teardown a second time.
                sim._health_reported = True
                problems += found
        if problems and not hasattr(report, "wasxfail"):
            report.outcome = "failed"
            report.longrepr = ("Simulator health check failed:\n"
                               + "\n".join(problems))
    if report.failed:
        _attach(item, report, sims)
    _quarantine(item, report)


def _quarantine(item, report):
    """A failing @pytest.mark.flaky test is reported as a quarantined flake
    (an xfail with a "quarantined flake" reason, listed in its own summary
    section) instead of failing the run. No retries.

    Never for a health failure: if any simulator the test used crashed,
    wrote a crash log or reported a sanitizer error, the failure stands."""
    marker = item.get_closest_marker("flaky")
    if not marker or not report.failed or report.when not in ("setup", "call"):
        return
    if any(sim.health_problems() for sim in _sims_of(item)):
        return
    reason = marker.kwargs.get("reason", "") or (marker.args[0] if marker.args else "")
    report.outcome = "skipped"
    report.wasxfail = f"quarantined flake: {reason}"


# ── Skip allow-list (audit C7) ──────────────────────────────────────────────

_skipped: list = []


def _allowlist() -> list[str]:
    if not SKIP_ALLOWLIST.exists():
        return []
    pats = []
    for line in SKIP_ALLOWLIST.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            pats.append(line)
    return pats


_quarantined: list = []


def pytest_runtest_logreport(report):
    if report.skipped and not hasattr(report, "wasxfail"):
        _skipped.append(report)
    if str(getattr(report, "wasxfail", "")).startswith("quarantined flake"):
        _quarantined.append(report)


def _skip_allowed(report, patterns) -> bool:
    if "hardware" in report.keywords or "asan_only" in report.keywords:
        return True
    nodeid = report.nodeid
    # Patterns are relative to tests/e2e so they work from any rootdir.
    short = nodeid.split("tests/e2e/", 1)[-1]
    return any(fnmatch.fnmatch(short, p) or fnmatch.fnmatch(nodeid, p)
               for p in patterns)


def pytest_sessionfinish(session, exitstatus):
    if hasattr(session.config, "workerinput"):
        return  # xdist worker: the controller sees every report
    patterns = _allowlist()
    bad = [r for r in _skipped if not _skip_allowed(r, patterns)]
    if bad:
        session.config._picos_bad_skips = bad
        if session.exitstatus == 0:
            session.exitstatus = pytest.ExitCode.TESTS_FAILED


def pytest_terminal_summary(terminalreporter, exitstatus, config):
    if _quarantined and not hasattr(config, "workerinput"):
        terminalreporter.section("quarantined flaky tests that failed", yellow=True)
        for r in _quarantined:
            terminalreporter.line(f"{r.nodeid} ({r.when}): {r.wasxfail}")
    bad = getattr(config, "_picos_bad_skips", [])
    if bad:
        terminalreporter.section("skips not on tests/e2e/skip_allowlist.txt",
                                 red=True)
        for r in bad:
            reason = r.longrepr[2] if isinstance(r.longrepr, tuple) else r.longrepr
            terminalreporter.line(f"{r.nodeid}: {reason}")
        terminalreporter.line(
            "A skip hides a test that no longer runs. Fix the cause, or add "
            "the test to tests/e2e/skip_allowlist.txt with a reason.")
