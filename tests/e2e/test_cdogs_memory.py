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
   here (and the existing binary must not be rebuilt), so this test reads
   the simulator subprocess's real stdout/stderr via the shared
   PicosSimulator harness's own get_output() (backed by its
   _start_pipe_drains() background threads, started automatically in
   sim.start()) and searches that text for HEAPSTAT/GFXSTAT lines instead
   of using get_log_buffer()/wait_for_log().

The one hazard get_output() doesn't remove on its own: its stdout/stderr
tails are each bounded at 2000 lines (collections.deque(maxlen=2000) in
picos_simulator.py), and C-Dogs' startup burst of "[TRAMP] fs_*" stderr
tracing (one line per file operation across ~2700 SD directory entries)
comfortably exceeds that in well under a second — easily enough volume to
evict an early HEAPSTAT/GFXSTAT line (in particular the very first,
boot-time report) from the tail before this module ever reads it.
_drive_quickplay below therefore polls get_output() throughout the whole
drive and accumulates every new matching line into a running list
(deduped by field values) instead of reading the tail once at the end —
a report is only lost if it's evicted before the very first poll after
it was written, never merely before the last one.

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

# Logged once via api->sys->log() in apps/cdogs/cdogs_picos.c, right
# before the menu's LoopRunnerRun() starts consuming input — the first
# point C-Dogs is actually ready to receive a keypress. _drive_quickplay
# waits for this literal line before sending any Enters (see its
# docstring for why: the boot-time HEAPSTAT/GFXSTAT report alone fires far
# too early, during font init, well before the campaign manifest scan
# that follows it finishes).
MENU_READY_MARKER = "CDOGS: Entering main menu loop"


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


def _combined_output(simulator):
    """Return the simulator's captured stdout+stderr as one string.

    HEAPSTAT/GFXSTAT are written to stderr (see apps/cdogs/picos_heap.h),
    but the simulator's own [TRAMP]/[UNICORN] trampoline tracing and the
    app's own logging don't reliably land on the same stream — combine
    both rather than assume which one a given marker is on.
    """
    out = simulator.get_output()
    return out["stdout"] + "\n" + out["stderr"]


def _poll_and_accumulate(simulator, parse_fn, seen, stats):
    """One poll of the simulator's output, merging any not-yet-seen
    matching lines into `stats` (in first-seen order), and returning the
    raw combined text polled.

    get_output()'s stdout/stderr tails are each bounded at 2000 lines
    (see module docstring), and C-Dogs' asset scan can emit thousands of
    "[TRAMP] fs_*" lines between two HEAPSTAT/GFXSTAT reports — easily
    enough to evict an earlier report (especially the very first,
    boot-time one) from the tail before anything ever reads it.
    Accumulating on every poll instead of parsing one snapshot at the end
    means a report is only ever lost if it's evicted before the very
    first poll to observe it, never merely before a later one. `seen` is
    a set of hashable field-tuples already recorded, so a report still
    present in both this poll's window and an earlier one isn't
    double-counted.
    """
    text = _combined_output(simulator)
    for stat in parse_fn(text):
        key = tuple(sorted(stat.items()))
        if key not in seen:
            seen.add(key)
            stats.append(stat)
    return text


# How often _sleep_and_accumulate re-polls while waiting. Deliberately
# tight: campaign/map/sprite loading after the third Enter can emit a
# dense burst of "[TRAMP] fs_*" lines (see module docstring), and the
# whole point of polling throughout the drive instead of once at the end
# is that a burst entirely contained within one gap between polls can
# still evict a report no poll ever saw. Empirically, blindly sleeping
# 0.8s between keypresses with no polling in between (the very gap this
# closes) was enough to occasionally lose the post-navigation report
# under ordinary host load — confirmed by reproducing the miss with a
# poll-only-at-the-edges variant of this same drive.
_POLL_INTERVAL_S = 0.05


def _sleep_and_accumulate(simulator, parse_fn, seen, stats, duration):
    """Sleep ~duration seconds, polling and accumulating every
    _POLL_INTERVAL_S throughout instead of once at the end or start.

    Used in place of a blind time.sleep() anywhere in the drive below
    that waits on the app doing work (e.g. between keypresses) — see
    _POLL_INTERVAL_S for why an unpolled sleep is unsafe here.
    """
    deadline = time.time() + duration
    while True:
        _poll_and_accumulate(simulator, parse_fn, seen, stats)
        remaining = deadline - time.time()
        if remaining <= 0:
            return
        time.sleep(min(_POLL_INTERVAL_S, remaining))


def _drive_quickplay(simulator, boot_marker, parse_fn, settle_s=12, done=None):
    """Launch C-Dogs, drive the quick-play menu flow, and return the parsed
    reports once navigation has demonstrably worked.

    Shared by test_heapstat_is_emitted_and_truthful and peak_gfx_total
    (issue #14) — both need the exact same launch / wait-for-boot-report /
    navigate / poll-until-settled sequence, only the report tag and parser
    differ.

    Real main menu structure (verified empirically by driving the
    simulator manually and screenshotting each step — see
    .superpowers/sdd/prereq-3-report.md for the captures). This replaced an
    older two-`enter` sequence that stopped working once verified against
    the actual UI:

        main menu: Start / Options... / Quit   ("Start" is default-selected)
          Enter -> "Start:" submenu:
            Campaign / Dogfight / Deathmatch / Join game (disabled) / Back
                                                 ("Campaign" is default-selected)
          Enter -> "Select a campaign:" list, one entry per campaign file
                                                 (first entry is default-selected)
          Enter -> loads it, producing further HEAPSTAT/GFXSTAT reports

    All three Enters land on an already-default-selected item, so no
    Down/Up presses are needed. That is also this drive's explicit
    layout dependency: if a menu item is ever added/reordered above
    "Start", "Campaign", or the first campaign in the list, the default
    selection at that level changes and this breaks — see the assertion
    message below, which names this exact dependency rather than failing
    silently.

    Sends the 3-Enter sequence up to twice: if the first attempt's report
    count never proves navigation worked, it retries once rather than
    failing immediately (self-healing, not a fixed-delay guess — see the
    retry loop's own comment for why resending 3 Enters is safe from any
    of the three levels this drive can get stuck at).

    done: optional predicate(stats) -> bool checked every poll of the
    settle loop; polling stops as soon as it returns True (or settle_s
    elapses, whichever first). Defaults to "at least two reports have
    arrived" — the signal that quick-play's post-navigation report
    actually landed, not just the boot-time one. Callers with a more
    specific completion signal (e.g. "peak has cleared a threshold") can
    supply their own so the loop breaks the moment that particular
    condition is satisfied rather than always waiting on report count.
    """
    if done is None:
        done = lambda stats: len(stats) > 1  # noqa: E731

    # Accumulated across every poll for the rest of this drive — see
    # _poll_and_accumulate's docstring for why a single end-of-drive read
    # of get_output() isn't safe against its bounded tails.
    seen = set()
    stats = []

    simulator.launch_app("cdogs")

    # Wait for the first report: a fast, reliable signal that the
    # instrumentation is wired up and C-Dogs has started booting. NOT a
    # signal that the main menu is ready for input — empirically this
    # fires during early graphics/font init (picos_asset_load_tick's very
    # first call always clears its tick gate), well before the campaign
    # manifest scan that follows it finishes.
    deadline = time.time() + 60
    text = ""
    while time.time() < deadline:
        text = _poll_and_accumulate(simulator, parse_fn, seen, stats)
        if stats:
            break
        time.sleep(_POLL_INTERVAL_S)
    assert stats, f"no {boot_marker} lines found in log:\n{text[-2000:]}"

    # The real "ready for input" signal: cdogs_picos.c logs this literal
    # line via api->sys->log() (routed to real stdout the same way
    # HEAPSTAT/GFXSTAT are — see module docstring) immediately before
    # entering the menu's LoopRunnerRun(), i.e. the first point input is
    # actually consumed. Sending Enters before this landed is the failure
    # mode that broke this drive previously: the campaign manifest scan
    # after the boot_marker tick can still be in flight, so an early Enter
    # is silently dropped (the app isn't polling input yet), leaving one
    # too few keypresses actually registered and the flow stuck one menu
    # level short of loading anything. Waiting for this line explicitly
    # replaces what used to be an assumption baked into fixed sleeps.
    # Keeps accumulating stats each poll too — the campaign manifest scan
    # (and any reports it produces) can still be in flight here.
    deadline = time.time() + 60
    while time.time() < deadline:
        text = _poll_and_accumulate(simulator, parse_fn, seen, stats)
        if MENU_READY_MARKER in text:
            break
        time.sleep(_POLL_INTERVAL_S)
    assert MENU_READY_MARKER in text, (
        f"'{MENU_READY_MARKER}' never appeared in the log — C-Dogs did not "
        f"reach an input-ready main menu:\n{text[-2000:]}"
    )

    # Two attempts: send Start > Campaign > first campaign, then poll for
    # the completion signal; if it never arrives, send the same 3 Enters
    # again before giving up. This is a genuine self-healing retry, not a
    # blind resend: from any of the three menu levels this drive can get
    # stuck at (main menu, "Start:" submenu, or the campaign list — e.g.
    # if the very first Enter races LoopRunnerRun's first input poll and
    # gets dropped, observed directly under host CPU contention from other
    # concurrent processes), 3 fresh Enters from wherever navigation
    # actually stalled always reaches a loaded campaign. A real menu-layout
    # drift, by contrast, reproduces identically on the retry and still
    # fails below with the same diagnostic.
    for attempt in range(2):
        for _ in range(3):
            simulator.keypress("enter")
            # Not a blind time.sleep(0.8): the third Enter (loading a
            # campaign) is exactly when the dense "[TRAMP] fs_*" burst
            # from campaign/map/sprite I/O happens, so this window must
            # keep polling throughout rather than only checking once
            # after the fact — see _POLL_INTERVAL_S.
            _sleep_and_accumulate(simulator, parse_fn, seen, stats, 0.8)

        # Poll for the completion signal, breaking out as soon as it's met
        # instead of sleeping the full window unconditionally — quick-play's
        # post-navigation report typically lands well under the settle_s
        # backstop.
        settle_deadline = time.time() + settle_s
        text = _poll_and_accumulate(simulator, parse_fn, seen, stats)
        while time.time() < settle_deadline and not done(stats):
            time.sleep(_POLL_INTERVAL_S)
            text = _poll_and_accumulate(simulator, parse_fn, seen, stats)

        if len(stats) > 1:
            break

    assert stats, f"no {boot_marker} lines found in log:\n{text[-2000:]}"

    # Navigation sanity check. The caller's own substantive assertions are
    # the only gate between "quick-play loaded" and a pass, and on their
    # own a failure there just says the expected data never showed up —
    # which points straight at the instrumentation (stubs.c / pic.c /
    # picos_heap.h). But the far more likely real cause is that the three
    # `enter` presses above no longer land on Start > Campaign > first
    # campaign (e.g. the main menu or a submenu gained/lost/reordered an
    # item and the default selection shifted): C-Dogs would then stay idle
    # after the first, boot-scan-only report and never produce a second
    # one, since quick-play is what drives the extra campaign/map/sprite
    # file I/O that triggers it. Catch that case here with a message that
    # names the actual suspect.
    assert len(stats) > 1, (
        f"only one {boot_marker} line observed after driving quick-play "
        "(3 Enters: Start > Campaign > first campaign in the list) — the "
        "main menu layout likely drifted from what this drive expects "
        "(main menu Start/Options/Quit -> \"Start:\" submenu "
        "Campaign/Dogfight/Deathmatch/Join game/Back -> \"Select a "
        "campaign:\" list; every Enter here relies on landing on an "
        "already-default-selected top item at that level) rather than a "
        f"{'heap' if boot_marker == 'HEAPSTAT' else 'graphics'}-instrumentation bug"
    )

    return stats


def test_heapstat_is_emitted_and_truthful(cdogs_simulator):
    """The heap gauge reports both readings, and true >= watermark.

    Guards against regressing to the watermark-only gauge, which silently
    under-reported free memory and mis-tuned the reserve guard.

    Drives the "Start" quick-play flow (see _drive_quickplay), which opens
    campaign/map/sprite data and reliably produces at least one more
    report after the heap has grown past the reserve threshold (verified
    directly: the resulting peak is consistently well above
    PEAK_RESERVE_THRESHOLD). This is the same instrumentation exercising
    the same code path the reserve guard itself exercises — not a separate
    scenario. The idle main menu alone never re-triggers file I/O, so
    without this drive only the single boot-time report would ever arrive
    and the peak field would go unexercised.
    """
    stats = _drive_quickplay(
        cdogs_simulator, "HEAPSTAT", parse_heapstats,
        done=lambda s: any(x["peak"] > PEAK_RESERVE_THRESHOLD for x in s),
    )

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

    Uses _drive_quickplay for the shared launch / wait-for-boot-report /
    navigate / poll-until-settled sequence (issue #14 — this used to carry
    its own near-identical copy of that logic) for the same reason
    test_heapstat_is_emitted_and_truthful does: only ~100 of 1683 PNGs are
    even attempted at the idle main menu (2 succeed, the rest are skipped
    by the reserve guard before a campaign is loaded) — the real sprite
    load happens on campaign entry. A plain launch-and-wait would see
    almost nothing.

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
    stats = _drive_quickplay(simulator, "GFXSTAT", parse_gfxstats, settle_s=settle_s)

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
