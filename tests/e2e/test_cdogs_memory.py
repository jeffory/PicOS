"""Memory instrumentation gates for the C-Dogs port.

Covers specs/2026-07-19-cdogs-asset-memory-design.md Stage 0 and Stage 1.

Why this fixture doesn't look like a plain launch-and-wait
------------------------------------------------------------
Two simulator-side gaps, previously latent because every other e2e
fixture app is a Lua app, made the straightforward version of this test
impossible to pass. Both are documented in full in task-1-report.md;
summary:

1. App list is scanned once at boot. launcher_run() (src/os/launcher.c)
   builds s_apps[] via scan_apps() once at startup; launch_app() over RPC
   (simulator/sim_socket_handler.c:h_launch_app) just queues a name and
   always returns {"ok": true} — the actual launcher_launch_by_name()
   lookup against s_apps[] happens later on the OS's own poll loop and
   silently does nothing if the name isn't in that list. So an app
   staged onto the SD card *after* the simulator process has started is
   invisible to launch_app(), even though the RPC call "succeeds". This
   fixture therefore stages C-Dogs and starts its own simulator instance
   (rather than reusing the shared `simulator` fixture, which is already
   running by the time a dependent fixture's body executes) so the app
   is present for the boot-time scan.

2. Native-app log() calls never reach get_log_buffer(). A native ELF
   app's sys->log() goes through simulator/unicorn_trampolines.c's
   tramp_sys_log(), which prints straight to the simulator process's own
   stdout and never calls sim_log_append() — unlike the Lua path
   (src/os/lua_bridge_sys.c's l_sys_log(), which explicitly also calls
   sim_log_append() when built with PICOS_SIMULATOR). get_log_buffer()
   only ever returns what sim_log_append() recorded, so it can never see
   a native app's log output, no matter how long wait_for_log() waits.
   Fixing the simulator side would need a rebuild, which is out of scope
   here (and the existing binary must not be rebuilt), so this test
   drains the simulator subprocess's real stdout/stderr pipes directly
   and searches that text for HEAPSTAT lines instead of using
   get_log_buffer()/wait_for_log().

That draining is also load-bearing for a third, unrelated reason: nothing
in the existing harness reads the simulator's stdout/stderr pipes at all,
and C-Dogs' startup burst of "[TRAMP] fs_*" stderr tracing (one line per
file operation across ~2700 SD directory entries) exceeds the OS pipe
buffer in well under a second. Once that pipe fills, the simulator
process blocks on write() — and since the RPC socket thread also prints
a debug line to stdout on every request, sustained RPC polling against
an undrained simulator eventually stalls entirely. Draining both pipes
from background threads as soon as the process starts avoids this.

Finally: picos_asset_load_tick()'s 5s report cadence (apps/cdogs/stubs.c)
is measured against sys->getTimeMs(), i.e. the simulator's uptime since
process start (SDL_GetTicks()) — not since C-Dogs launches. C-Dogs' own
startup scan of data/graphics plus the campaign/dogfight lists finishes
in well under a second in this simulator (direct host filesystem
passthrough, no real SD card latency), faster than the 5s cadence, and
the app then idles at the main menu doing no further file I/O. Unless
the simulator has already been up 5s+ before the scan starts, no
HEAPSTAT line is ever emitted — confirmed empirically. This fixture
waits for that headroom before launching so the cadence's first eligible
tick fires during the scan.
"""
import re
import shutil
import threading
import time
from pathlib import Path

import pytest

from picos_simulator import PicosSimulator

PROJECT_ROOT = Path(__file__).resolve().parents[2]
CDOGS_SRC = PROJECT_ROOT / "apps" / "cdogs"

HEAPSTAT_RE = re.compile(
    r"HEAPSTAT (\S+) watermark=(\d+) true=(\d+) arena=(\d+) used=(\d+)"
)


def _drain(stream, sink, lock):
    """Read a subprocess pipe to EOF, appending decoded lines to sink.

    Line-based on purpose: BufferedReader.read(n) on a non-interactive
    pipe keeps issuing raw reads until it gathers n bytes or hits EOF, so
    with a fixed size it can block indefinitely once the app goes quiet
    even though bytes are already sitting in the pipe. readline() returns
    as soon as a line is available instead.
    """
    try:
        for raw in iter(stream.readline, b""):
            with lock:
                sink.append(raw.decode("utf-8", errors="replace"))
    except (ValueError, OSError):
        pass  # stream closed under us during simulator teardown


def start_stdio_drain(simulator):
    """Continuously drain the simulator subprocess's stdout and stderr.

    Returns a callable that snapshots everything captured so far as one
    string. See the module docstring for why this is necessary instead of
    get_log_buffer()/wait_for_log().
    """
    lines = []
    lock = threading.Lock()
    for stream in (simulator.process.stdout, simulator.process.stderr):
        t = threading.Thread(target=_drain, args=(stream, lines, lock), daemon=True)
        t.start()

    def snapshot():
        with lock:
            return "".join(lines)

    return snapshot


@pytest.fixture
def cdogs_simulator(simulator_binary, test_sd_card, request):
    """Simulator with C-Dogs staged onto the SD card before boot.

    See the module docstring for why this doesn't reuse the shared
    `simulator` fixture and doesn't rely on get_log_buffer()/wait_for_log().
    """
    if not (CDOGS_SRC / "main.elf").exists():
        pytest.skip("apps/cdogs/main.elf not built — run `make` in apps/cdogs")
    if not (CDOGS_SRC / "data" / "graphics").exists():
        pytest.skip("apps/cdogs/data not prepared — run ./prepare_data.sh")

    dest = Path(test_sd_card) / "apps" / "cdogs"
    dest.mkdir(parents=True, exist_ok=True)
    for name in ("main.elf", "app.json"):
        shutil.copy2(CDOGS_SRC / name, dest / name)
    shutil.copytree(CDOGS_SRC / "data", dest / "data", dirs_exist_ok=True)

    headless = request.config.getoption("--headless")
    port = request.config.getoption("--port")
    sim = PicosSimulator(
        binary_path=str(simulator_binary),
        sd_card_path=str(test_sd_card),
        headless=headless,
        tcp_port=port,
    )
    sim.start()

    # Start draining before launch so the pipes never get a chance to
    # back up (see module docstring).
    sim.stdio_snapshot = start_stdio_drain(sim)

    # Wait for enough simulator uptime that the report cadence's first
    # eligible tick fires during C-Dogs' (fast) asset scan (see module
    # docstring).
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            status = sim.call("ping", timeout=2.0)
        except Exception:
            status = {}
        if status.get("uptime_ms", 0) >= 5500:
            break
        time.sleep(0.2)

    yield sim
    sim.stop()


def read_log_text(simulator, since_seq=0):
    """Concatenate the simulator log buffer into one searchable string.

    get_log_buffer returns {lines: [...], next_seq: int}, and each line is
    either a bare string or a dict carrying a "text" key.

    Not used by the native-app test below (see module docstring) — kept
    for Lua-side instrumentation in later stages of this spec.
    """
    result = simulator.get_log_buffer(since_seq=since_seq)
    lines = [
        line if isinstance(line, str) else line.get("text", "")
        for line in result.get("lines", [])
    ]
    return "\n".join(lines)


def parse_heapstats(log_text):
    """Return list of dicts for every HEAPSTAT line in the log."""
    out = []
    for m in HEAPSTAT_RE.finditer(log_text):
        out.append({
            "tag": m.group(1),
            "watermark": int(m.group(2)),
            "true": int(m.group(3)),
            "arena": int(m.group(4)),
            "used": int(m.group(5)),
        })
    return out


def test_heapstat_is_emitted_and_truthful(cdogs_simulator):
    """The heap gauge reports both readings, and true >= watermark.

    Guards against regressing to the watermark-only gauge, which silently
    under-reported free memory and mis-tuned the reserve guard.
    """
    cdogs_simulator.launch_app("cdogs")

    deadline = time.time() + 60
    text = ""
    while time.time() < deadline:
        text = cdogs_simulator.stdio_snapshot()
        if "HEAPSTAT" in text:
            break
        time.sleep(0.2)

    stats = parse_heapstats(text)

    assert stats, f"no HEAPSTAT lines found in log:\n{text[-2000:]}"
    for s in stats:
        assert s["true"] >= s["watermark"], (
            f"true free ({s['true']}) below watermark ({s['watermark']}) "
            f"at tag {s['tag']} — arithmetic is wrong"
        )
        assert s["true"] <= 5 * 1024 * 1024, (
            f"true free ({s['true']}) exceeds the 5MB arena at tag {s['tag']}"
        )
