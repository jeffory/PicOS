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

Finally: picos_asset_load_tick()'s 1000ms report cadence (apps/cdogs/stubs.c)
is measured against sys->getTimeMs(), i.e. the simulator's uptime since
process start (SDL_GetTicks()) — not since C-Dogs launches. C-Dogs' own
startup scan of data/graphics plus the campaign/dogfight lists finishes
in well under a second in this simulator (direct host filesystem
passthrough, no real SD card latency), faster than the 1000ms cadence, and
the app then idles at the main menu doing no further file I/O. Unless
the simulator has already been up 1000ms+ before the scan starts, no
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
    r"HEAPSTAT (\S+) watermark=(\d+) true=(\d+) arena=(\d+) used=(\d+) peak=(\d+)"
)

# Mirrors IMG_LOAD_HEAP_RESERVE in apps/cdogs/src/src/cdogs/utils.c
# (2560 * 1024 bytes = 2.5 MiB) — the real reserve guard the peak
# assertion below exists to prove fired. Keep this in sync if that
# constant ever changes.
PEAK_RESERVE_THRESHOLD = 2_621_440


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
    # docstring). The cadence itself is 1000ms, so 1100ms is enough
    # headroom for the first tick to land inside the scan.
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            status = sim.call("ping", timeout=2.0)
        except Exception:
            status = {}
        if status.get("uptime_ms", 0) >= 1100:
            break
        time.sleep(0.2)

    yield sim

    # The autouse _check_crash_log fixture in conftest.py depends on the
    # shared `simulator` fixture, not this one — every test in this module
    # uses cdogs_simulator instead, so that autouse check silently inspects
    # a second, unused simulator instance and never looks at this one. This
    # is the only native ELF app under e2e test, in exactly the
    # memory-fragile regime this test module exists to cover, so check here
    # explicitly before tearing down. try/finally so a crash (or a failed
    # get_crash_log call) still lets sim.stop() run and reap the process.
    try:
        crash = sim.call("get_crash_log", timeout=2.0).get("crash_log")
        assert not crash, f"C-Dogs simulator crashed during test:\n{crash}"
    finally:
        sim.stop()


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
            "peak": int(m.group(6)),
        })
    return out


def test_heapstat_is_emitted_and_truthful(cdogs_simulator):
    """The heap gauge reports both readings, and true >= watermark.

    Guards against regressing to the watermark-only gauge, which silently
    under-reported free memory and mis-tuned the reserve guard.
    """
    cdogs_simulator.launch_app("cdogs")

    # Wait for the first HEAPSTAT line: a fast, reliable signal that
    # C-Dogs has booted and reached the main menu.
    deadline = time.time() + 60
    text = ""
    while time.time() < deadline:
        text = cdogs_simulator.stdio_snapshot()
        if "HEAPSTAT" in text:
            break
        time.sleep(0.2)

    assert "HEAPSTAT" in text, f"no HEAPSTAT lines found in log:\n{text[-2000:]}"

    # That first report is not enough on its own to exercise the peak
    # field. Measured directly against this simulator (fast host-FS
    # passthrough, no simulated SD latency — see module docstring): the
    # entire boot-time asset scan (font/wall graphics through the
    # LoadImgToSurface reserve-guard SKIPs, campaign/dogfight manifest
    # reads, down to "Entering main menu loop") completes in well under
    # 500ms of wall-clock time. picos_asset_load_tick's very first call of
    # the whole process always clears its 500ms tick gate (the static
    # s_last_tick_ms starts at 0), so exactly one HEAPSTAT line fires
    # during that scan — and empirically it lands a few dozen ms *before*
    # the heap actually crosses the 2.5MB reserve, not after. Once C-Dogs
    # reaches the main menu it goes fully idle (no further file I/O at
    # all), so no second report would ever follow no matter how long this
    # loop waits — the peak tracker's "captured between reports" design
    # only pays off if some later report actually happens.
    #
    # So: drive the "Start" quick-play flow, which opens further
    # campaign/map/sprite data and reliably produces at least one more
    # report after the heap has grown past the reserve threshold (verified
    # directly: the resulting peak is consistently 3044520 across repeated
    # runs). This is the same instrumentation exercising the same code
    # path the reserve guard itself exercises — not a separate scenario.
    cdogs_simulator.keypress("enter")
    time.sleep(0.8)
    cdogs_simulator.keypress("enter")

    # Poll for a HEAPSTAT report whose peak has already cleared the reserve
    # threshold, breaking out as soon as it shows up, instead of sleeping
    # the full window unconditionally — quick-play's post-navigation
    # report typically lands well under the 12s backstop below.
    settle_deadline = time.time() + 12
    text = cdogs_simulator.stdio_snapshot()
    stats = parse_heapstats(text)
    while time.time() < settle_deadline and not any(
        s["peak"] > PEAK_RESERVE_THRESHOLD for s in stats
    ):
        time.sleep(0.3)
        text = cdogs_simulator.stdio_snapshot()
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

    # The actual Stage 0 gate (specs/2026-07-19-cdogs-asset-memory-design.md):
    # true-free and watermark must DIFFER, not just satisfy true >= watermark.
    # true==watermark everywhere (e.g. if mallinfo().fordblks were always 0,
    # as it would be under the old watermark-only gauge this instrumentation
    # replaced) would still pass the >= check above while proving nothing —
    # this is the only assertion that actually proves newlib is recycling
    # freed blocks the watermark-only gauge couldn't see.
    assert max(s["true"] - s["watermark"] for s in stats) > 0, (
        "true free was never greater than watermark across any HEAPSTAT "
        "line — this is supposed to prove newlib recycles freed blocks "
        "(via mallinfo().fordblks) that the sbrk watermark alone cannot "
        "see; if this never diverges, the 'true' gauge isn't measuring "
        "anything the old watermark-only gauge didn't already"
    )

    # Navigation sanity check. The peak assertion below is the only gate
    # between "quick-play loaded" and a pass, and on its own a failure
    # there just says the heap never filled — which points straight at
    # stubs.c. But the far more likely real cause is that the two `enter`
    # keypresses above no longer land on "Start" (e.g. the main menu
    # gained/lost an item and the layout shifted): C-Dogs would then stay
    # idle after the first, boot-scan-only HEAPSTAT report and never
    # produce a second one, since quick-play is what drives the extra
    # campaign/map/sprite file I/O that triggers it. Catch that case here
    # with a message that names the actual suspect.
    assert len(stats) > 1, (
        "only one HEAPSTAT line observed after the quick-play keypresses — "
        "the two 'enter' presses likely didn't land on \"Start\" (main menu "
        "layout may have drifted) rather than a heap-instrumentation bug"
    )

    # The real regression gate: the old watermark-on-a-5s-cadence gauge
    # could (and did) sample exactly once, before loading even started,
    # and never witness the heap growing past the 2.5 MiB LoadImgToSurface
    # reserve threshold. The peak tracker is sampled on every _sbrk()
    # growth, not just at report time, so it must have observed the heap
    # actually filling up regardless of sampling cadence.
    max_peak = max(s["peak"] for s in stats)
    assert max_peak > PEAK_RESERVE_THRESHOLD, (
        f"max observed peak ({max_peak}) never exceeded the 2.5 MiB reserve "
        f"threshold ({PEAK_RESERVE_THRESHOLD}) — instrumentation did not "
        f"witness the heap filling"
    )


GFXSTAT_RE = re.compile(
    r"GFXSTAT (\S+) pics=(-?\d+) data=(\d+) tex=(\d+) total=(\d+) peak=(\d+) skipped=(-?\d+)"
)


def parse_gfxstats(log_text):
    """Return list of dicts for every GFXSTAT line in the log."""
    out = []
    for m in GFXSTAT_RE.finditer(log_text):
        out.append({
            "tag": m.group(1),
            "pics": int(m.group(2)),
            "data": int(m.group(3)),
            "tex": int(m.group(4)),
            "total": int(m.group(5)),
            "peak": int(m.group(6)),
            "skipped": int(m.group(7)),
        })
    return out


def peak_gfx_total(simulator, settle_s=12):
    """Launch cdogs, drive quick-play, and return the GFXSTAT line whose
    peak= field equals the overall observed high-water mark.

    Mirrors test_heapstat_is_emitted_and_truthful's drive sequence and for
    the same reason: only ~100 of 1683 PNGs are even attempted at the idle
    main menu (2 succeed, the rest are skipped by the reserve guard before
    a campaign is loaded) — the real sprite load happens on campaign entry.
    A plain launch-and-wait would see almost nothing. So this sends the
    same two 'enter' keypresses that land on "Start" for quick-play, then
    polls stdio for GFXSTAT lines.

    peak= is a running high-water mark of (data+tex) sampled every time
    either byte counter grows (see picos_gfx_bytes_peak_sample in
    picos_heap.h) — not just at report time — so unlike a plain instant
    sample it cannot land between two ticks and miss the load entirely.

    Because peak= is monotonically non-decreasing, max(stats, key=...)
    returns the FIRST line that reached the final peak value, not
    necessarily the last one — any later line sharing that same peak value
    is skipped over. Only the returned dict's peak field should be treated
    as meaningful: it genuinely is the high-water mark. Its pics/data/tex/
    total/skipped fields are just that one tick's own snapshot (recomputed
    at report time), not the state at the instant the peak was actually
    set — e.g. data/tex can shrink afterward (PicShrink) while peak holds
    still. Callers that need the settled end-state should look at the last
    element of the parsed stats list instead of this function's return
    value.
    """
    simulator.launch_app("cdogs")

    # Wait for the first GFXSTAT line: confirms C-Dogs has booted and the
    # instrumentation is wired up before driving quick-play.
    deadline = time.time() + 60
    text = ""
    while time.time() < deadline:
        text = simulator.stdio_snapshot()
        if "GFXSTAT" in text:
            break
        time.sleep(0.2)
    assert "GFXSTAT" in text, f"no GFXSTAT lines found in log:\n{text[-2000:]}"

    simulator.keypress("enter")
    time.sleep(0.8)
    simulator.keypress("enter")

    # Settle window: give quick-play's campaign/map/sprite load time to
    # run and be observed by at least one more report tick.
    settle_deadline = time.time() + settle_s
    text = simulator.stdio_snapshot()
    stats = parse_gfxstats(text)
    while time.time() < settle_deadline:
        time.sleep(0.3)
        text = simulator.stdio_snapshot()
        stats = parse_gfxstats(text)

    assert stats, f"no GFXSTAT lines found in log:\n{text[-2000:]}"

    # Navigation sanity check, mirroring test_heapstat_is_emitted_and_truthful's
    # guard. The substantive assertions the caller runs against this
    # function's return value (pics>0, data>0, peak>0, total==data+tex) are
    # all satisfiable by the boot-time asset scan alone — the idle main menu
    # already loads ~100 of 1683 PNGs before quick-play is ever driven (see
    # this function's docstring) — so none of them prove quick-play was
    # actually reached. A failure there just says the graphics accounting
    # looks wrong, which points straight at pic.c / picos_heap.h. But the
    # far more likely real cause is that the two `enter` keypresses above no
    # longer land on "Start" (e.g. the main menu gained/lost an item and the
    # layout shifted): C-Dogs would then stay idle after the first,
    # boot-scan-only GFXSTAT report and never produce a second one, since
    # quick-play is what drives the campaign/map/sprite load that grows
    # pics/data/tex further. Catch that case here, before the caller's
    # substantive assertions run, with a message that names the actual
    # suspect.
    assert len(stats) > 1, (
        "only one GFXSTAT line observed after the quick-play keypresses — "
        "the two 'enter' presses likely didn't land on \"Start\" (main menu "
        "layout may have drifted) rather than a graphics-instrumentation bug"
    )

    return max(stats, key=lambda s: s["peak"])


def test_gfxstat_reports_resident_graphics(cdogs_simulator):
    """Resident graphics accounting is emitted and internally consistent.

    Originally established the Stage 1 baseline for Task 4 (collapsing the
    Data/Tex duplication), back when total was roughly 2x data because
    every pic held two identical copies. Task 4 has since landed (textures
    borrow Pic->Data instead of copying it — see
    test_textures_borrow_rather_than_duplicate below), so that 2x
    relationship no longer holds and this docstring no longer claims it.
    Asserts internal consistency and that a peak was actually observed, not
    any specific byte figure — the point of this test is to measure the
    current footprint, not pin it.
    """
    peak = peak_gfx_total(cdogs_simulator)

    assert peak["total"] == peak["data"] + peak["tex"], (
        f"total {peak['total']} != data {peak['data']} + tex {peak['tex']}"
    )
    assert peak["pics"] > 0, "no pics counted"
    assert peak["data"] > 0, "no pic data counted"
    assert peak["peak"] > 0, "no peak recorded"


EXCLUDED_PATTERNS = ("*.blend", "*.blend1", "render.py",
                     "make_spritesheet.sh", "README.md")


def test_sd_payload_excludes_non_runtime_sources():
    """Staged game data carries no Blender sources or build scripts.

    These are ~19MB of the 27MB graphics tree and are never opened at
    runtime; they also inflate directory entry counts during asset scans.
    """
    data_dir = CDOGS_SRC / "data"
    if not data_dir.exists():
        pytest.skip("apps/cdogs/data not prepared — run ./prepare_data.sh")

    offenders = []
    for pattern in EXCLUDED_PATTERNS:
        offenders.extend(str(p.relative_to(data_dir))
                         for p in data_dir.rglob(pattern))

    assert not offenders, (
        f"{len(offenders)} non-runtime files staged, e.g. {offenders[:5]}"
    )


def test_textures_borrow_rather_than_duplicate(cdogs_simulator):
    """Textures alias Pic->Data instead of holding a second copy.

    With no GPU a texture is plain heap, so duplicating every image
    doubled resident graphics memory for no benefit. tex_bytes accounting
    was moved from pic.c (caller) into the SDL texture shim itself
    (picos_sdl_impl.c's SDL_CreateTexture/SDL_DestroyTexture) so it counts
    bytes a texture actually OWNS — PicosTextureBorrow (used for every
    per-pic texture on this port) adds nothing. That means tex is NOT
    expected to be 0: a handful of textures legitimately own their pixels
    — grafx.c's GraphicsInitialize creates 5 whole-screen ARGB8888 buffers
    (bkgTgt, bkg, screen, hud, brightnessOverlay) via SDL_CreateTexture,
    none of which are per-pic sprite copies. See TEX_CEILING_BYTES below
    for the measured legitimate baseline and how the ceiling was chosen.
    """
    peak = peak_gfx_total(cdogs_simulator)

    # Measured on this simulator (deterministic across repeated runs — these
    # are fixed 320x240 ARGB8888 window buffers created once during
    # GraphicsInitialize, independent of how many sprites load):
    #   5 owning textures * 320 * 240 * 4 bytes = 1_536_000
    # If a per-pic duplication path were reintroduced (i.e. Task 3's
    # regression), tex would additionally gain roughly one more copy of
    # `data` per pic — using this run's own data=81824 as the estimate,
    # that's ~1_536_000 + 81_824 ~= 1_617_824. The ceiling below sits
    # roughly halfway between the measured legitimate baseline and that
    # regression estimate: generous headroom over what's actually observed,
    # but comfortably below the point a reintroduced duplication path would
    # reach.
    TEX_CEILING_BYTES = 1_580_000
    assert peak["tex"] < TEX_CEILING_BYTES, (
        f"tex holds {peak['tex']} bytes, expected under {TEX_CEILING_BYTES} "
        "— legitimate owning textures (grafx.c's window-sized render "
        "buffers) measured at 1_536_000 on this simulator; a figure "
        "meaningfully above that suggests a per-pic texture-duplication "
        "path was reintroduced somewhere"
    )
    assert peak["data"] > 0, "no pic data counted; accounting is broken"

    # Secondary, heap-pressure-immune gate. Freeing the duplicate texture
    # copy relaxes the reserve guard (utils.c's IMG_LOAD_HEAP_RESERVE), so
    # it now skips fewer images than Task 3's run did — pics rises, and raw
    # data/peak can hold steady or even grow instead of halving. Bytes per
    # pic is not sensitive to how many images got past the guard: Task 3's
    # baseline was ~320 bytes/pic (163648/512, tex==data duplication in
    # full); with textures borrowed there is only one copy per pic, so this
    # should roughly halve to ~160. Uses `data` rather than `total` here
    # deliberately: `total` now includes the fixed, non-per-pic window-
    # buffer bytes accounted for above, which would swamp this ratio and
    # defeat its purpose. Assert a generous ceiling rather than pin an
    # exact figure.
    assert peak["pics"] > 0, "no pics counted; accounting is broken"
    bytes_per_pic = peak["data"] / peak["pics"]
    assert bytes_per_pic < 240, (
        f"data/pics = {bytes_per_pic:.1f} bytes/pic — expected roughly half "
        f"of Task 3's ~320 baseline once the duplicate texture copy is gone"
    )
