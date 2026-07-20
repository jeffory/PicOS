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

Fixture structure (prereq-6, 2026-07-20)
------------------------------------------------------------
cdogs_simulator now boots and stages C-Dogs ONCE for the whole module
instead of once per test. It is by far the most expensive fixture in this
file — C-Dogs runs under Unicorn CPU emulation, and staging it means a
~22MB copytree of the game data tree onto a fresh SD card — and every
test below only ever *reads* the diagnostics one quick-play drive
produces; none of them mutate simulator state in a way that would need a
fresh instance. Repeating that boot/stage/drive per test bought nothing
but 4x the wall-clock cost and, worse, 4 independent per-run chances to
hit the navigation flake _drive_quickplay documents below (a keypress
dropped under host contention). cdogs_quickplay_stats drives quick-play
exactly once and hands every test the same parsed HEAPSTAT and GFXSTAT
report lists; each test still applies its own assertions, with its own
messages and thresholds, against that shared data — a real regression in
any one of them still fails on its own, legibly. Only the drive itself is
shared, not the pass/fail verdicts. See .superpowers/sdd/prereq-6-report.md
for the before/after measurements.
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


# Pre-existing debug instrumentation in picos_sdl_impl.c's SDL_RenderPresent
# (not added for this test): it fprintf(stderr, ...)s a per-frame pixel
# census — how many of the just-presented framebuffer's pixels are non-zero
# — for the first 8 SDL_RenderPresent calls of the process, then goes
# silent. Reused here (see test_boot_loading_screen_is_not_blank) instead
# of polling the display_stats RPC because it's synchronous with the exact
# frame it describes: RPC-polling display_stats independently was tried
# first and proved unreliable for this purpose — it can observe the
# PicOS launcher's own leftover screen content from before C-Dogs ever
# presented a frame (nothing has overwritten the panel yet at that point),
# which reads as "non-blank" regardless of whether C-Dogs' own render path
# is healthy. This log line has no such gap: it's computed from the exact
# buffer C-Dogs itself just presented, at the moment it presented it.
RENDERPRESENT_RE = re.compile(
    r"RenderPresent #(\d+): (\d+)x(\d+) colored=(\d+)/(\d+) "
    r"first@\((-?\d+),(-?\d+)\)=0x([0-9A-Fa-f]{4})"
)


def parse_renderpresents(log_text):
    """Return list of dicts for every RenderPresent debug line in the log,
    in the order SDL_RenderPresent was called (its numbering starts at 1
    and is never reused within one process lifetime)."""
    out = []
    for m in RENDERPRESENT_RE.finditer(log_text):
        out.append({
            "num": int(m.group(1)),
            "w": int(m.group(2)),
            "h": int(m.group(3)),
            "colored": int(m.group(4)),
            "total": int(m.group(5)),
        })
    return out


# Floor proving the peak tracker actually witnessed substantial heap
# growth during asset loading, not just the boot-time sample. This used
# to mirror IMG_LOAD_HEAP_RESERVE (utils.c's 2.5 MiB reserve guard) to
# prove that guard had tripped, but freeing memory elsewhere (the shim's
# whole-screen textures moving from ARGB8888 to RGB565) relaxed the guard
# enough that it no longer trips at all — this constant is NOT about that
# guard anymore, only about proving the instrumentation samples the
# loaded state and not just boot. Chosen comfortably above the ~488_000-
# byte boot-time peak (font/early init, before any campaign data loads)
# and comfortably below the loaded peak actually observed once quick-play
# loads a campaign (~2.6 MiB as of this writing — see
# .superpowers/sdd/task-3-report.md for the measured before/after
# figures), so it still fails loudly if this ever regresses to sampling
# only the boot-time state.
LOADED_PEAK_FLOOR_BYTES = 1_500_000

# 5 whole-screen 320x240 window textures at 2 bytes per pixel (RGB565).
# Fixed in number and size, so this figure is deterministic.
ALL_16BIT_TEX_BYTES = 5 * 320 * 240 * 2  # 768_000

# Logged once via api->sys->log() in apps/cdogs/cdogs_picos.c, right
# before the menu's LoopRunnerRun() starts consuming input — the first
# point C-Dogs is actually ready to receive a keypress. _drive_quickplay
# waits for this literal line before sending any Enters (see its
# docstring for why: the boot-time HEAPSTAT/GFXSTAT report alone fires far
# too early, during font init, well before the campaign manifest scan
# that follows it finishes).
MENU_READY_MARKER = "CDOGS: Entering main menu loop"


@pytest.fixture(scope="module")
def cdogs_simulator(simulator_binary, tmp_path_factory, request):
    """Simulator with C-Dogs staged onto the SD card before boot.

    Module-scoped: booted and staged exactly once for every test in this
    file (see "Fixture structure" in the module docstring for why). Stages
    its own SD card via the session-scoped tmp_path_factory rather than
    conftest.py's function-scoped `test_sd_card` fixture, which a
    module-scoped fixture cannot depend on (pytest scope mismatch) —
    otherwise this mirrors test_sd_card's construction exactly (default SD
    card contents + tests/e2e/apps/* fixture apps), plus C-Dogs on top.

    See the module docstring for why this doesn't reuse the shared
    `simulator` fixture and doesn't rely on get_log_buffer()/wait_for_log().
    """
    if not (CDOGS_SRC / "main.elf").exists():
        pytest.skip("apps/cdogs/main.elf not built — run `make` in apps/cdogs")
    if not (CDOGS_SRC / "data" / "graphics").exists():
        pytest.skip("apps/cdogs/data not prepared — run ./prepare_data.sh")

    sd_path = tmp_path_factory.mktemp("cdogs_sd_card")

    default_sd = Path(request.config.getoption("--sd-card-path"))
    if default_sd.exists():
        shutil.copytree(default_sd, sd_path, dirs_exist_ok=True)
    (sd_path / "apps").mkdir(exist_ok=True)
    (sd_path / "data").mkdir(exist_ok=True)
    (sd_path / "system").mkdir(exist_ok=True)

    fixture_apps = Path(__file__).parent / "apps"
    if fixture_apps.exists():
        for app_dir in fixture_apps.iterdir():
            if app_dir.is_dir():
                dest = sd_path / "apps" / app_dir.name
                if not dest.exists():
                    shutil.copytree(app_dir, dest)

    dest = sd_path / "apps" / "cdogs"
    dest.mkdir(parents=True, exist_ok=True)
    for name in ("main.elf", "app.json"):
        shutil.copy2(CDOGS_SRC / name, dest / name)
    shutil.copytree(CDOGS_SRC / "data", dest / "data", dirs_exist_ok=True)

    headless = request.config.getoption("--headless")
    port = request.config.getoption("--port")
    sim = PicosSimulator(
        binary_path=str(simulator_binary),
        sd_card_path=str(sd_path),
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
    # uses cdogs_simulator (via cdogs_quickplay_stats) instead, so that
    # autouse check silently inspects a second, unused simulator instance
    # and never looks at this one. This is the only native ELF app under
    # e2e test, in exactly the memory-fragile regime this test module
    # exists to cover, so check here explicitly before tearing down.
    # try/finally so a crash (or a failed get_crash_log call) still lets
    # sim.stop() run and reap the process. Module-scoped now means this
    # runs once, after the last test in the module that needed this
    # fixture, rather than once per test — a crash occurring after the
    # single shared drive (during one test's own assertions, which only
    # read already-collected data and touch nothing on the simulator) is
    # exceedingly unlikely to surface only there and not already have
    # broken the drive itself, but see prereq-6-report.md for the
    # reasoning in full.
    try:
        crash = sim.call("get_crash_log", timeout=2.0).get("crash_log")
        assert not crash, f"C-Dogs simulator crashed during test:\n{crash}"
    finally:
        sim.stop()


def _combined_output(simulator):
    """Return the simulator's captured stdout+stderr as one string.

    HEAPSTAT/GFXSTAT are written to stderr (see apps/cdogs/picos_heap.h),
    but the simulator's own [TRAMP]/[UNICORN] trampoline tracing and the
    app's own logging don't reliably land on the same stream — combine
    both rather than assume which one a given marker is on.
    """
    out = simulator.get_output()
    return out["stdout"] + "\n" + out["stderr"]


def _accumulate(text, parse_fn, seen, stats):
    """Merge any not-yet-seen parse_fn(text) matches into `stats` (in
    first-seen order). `seen` is a set of hashable field-tuples already
    recorded, so a report still present in both this poll's window and an
    earlier one isn't double-counted.

    get_output()'s stdout/stderr tails are each bounded at 2000 lines (see
    module docstring), and C-Dogs' asset scan can emit thousands of
    "[TRAMP] fs_*" lines between two HEAPSTAT/GFXSTAT reports — easily
    enough to evict an earlier report (especially the very first,
    boot-time one) from the tail before anything ever reads it.
    Accumulating on every poll instead of parsing one snapshot at the end
    means a report is only ever lost if it's evicted before the very
    first poll to observe it, never merely before a later one.
    """
    for stat in parse_fn(text):
        key = tuple(sorted(stat.items()))
        if key not in seen:
            seen.add(key)
            stats.append(stat)


# How often the drive re-polls get_output() while waiting. Deliberately
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

# Throttle for the display_stats RPC calls _screen_signature makes below.
# Separate from _POLL_INTERVAL_S because, unlike a local deque read for
# get_output(), display_stats is a network round trip to the simulator
# (albeit a cheap one — a single framebuffer scan server-side, no PNG
# encode/decode the way screenshot_pil() would need; see
# simulator/sim_socket_handler.c's h_display_stats). Menu redraws don't
# happen faster than this, so polling it every _POLL_INTERVAL_S (50ms)
# would just multiply RPC traffic for no extra sensitivity.
_SCREEN_POLL_INTERVAL_S = 0.15


def _screen_signature(simulator):
    """Cheap fingerprint of the current framebuffer, used as evidence of
    what's currently on screen.

    Uses the display_stats RPC (the same one test_display_colors.py
    exercises) rather than screenshot_pil(): it's a single framebuffer
    scan on the simulator side with no PNG encode/decode round trip, so
    it's cheap enough to poll repeatedly during a wait. Combines
    unique_colors, nonzero_pixels, and the first non-zero pixel's
    position/value into one tuple — main-menu-family screens in this app
    reportedly hold ~24 distinct colours (including a magenta accent,
    0xB817) while the post-load mission briefing is a sparse white-on-
    black screen with only ~4, but this drive doesn't need to know which
    specific screen it's looking at: any two visually different screens
    are highly likely to differ in at least one of these fields.

    NOT safe to compare directly against a single prior sample as "did
    the screen change": C-Dogs' menu screens animate a blinking selection
    cursor at roughly 15-30Hz even with no input at all (confirmed
    empirically — see .superpowers/sdd/prereq-5-report.md), so two
    consecutive samples of the very same idle screen routinely differ.
    Callers need _learn_screen_baseline's idle-set comparison, not a
    naive prev-vs-current diff, to tell a real transition apart from that
    animation.

    Returns None if the RPC call fails for any reason (e.g. a transient
    hiccup, or the app briefly not rendering between frames). Callers
    must treat None as inconclusive — neither "matches the baseline" nor
    "differs from it".
    """
    try:
        stats = simulator.call("display_stats", timeout=3.0)
    except Exception:
        return None
    first_nonzero = stats.get("first_nonzero") or {}
    return (
        stats.get("unique_colors"),
        stats.get("nonzero_pixels"),
        first_nonzero.get("rgb565"),
        first_nonzero.get("x"),
        first_nonzero.get("y"),
    )


# _learn_screen_baseline stops once the observed signature set has gone
# this many consecutive samples without gaining a new value ("stale") —
# not after a fixed clock duration. A fixed duration (this drive's first
# attempt at this) undersamples under host load: display_stats round trips
# get slower, so fewer samples fit in any fixed window, and a screen whose
# idle blink has (say) 2 states can easily get caught with only 1 of them
# characterized — the nudge loop then mistakes the screen's OWN other
# blink phase for a real transition the moment it shows up, firing an
# extra Enter mid-load, which is a genuinely unsafe moment for one (unlike
# a menu at rest, "extra Enters are harmless" does not obviously hold
# while a campaign is actively loading) — reproduced directly under
# deliberate host load (`yes > /dev/null`) while developing this drive;
# see .superpowers/sdd/prereq-5-report.md. Requiring the set to actually
# stop growing for a streak of samples, however long that takes in wall
# time, removes the guesswork: it costs a little more time on a slow
# system and almost none on a fast one, rather than being wrong on a slow
# one. _BASELINE_MAX_DURATION_S is still a hard backstop in case a screen
# never truly settles (e.g. baseline-learning starts mid-transition and
# the screen keeps changing) so this can't wait forever.
_BASELINE_STABLE_STREAK = 6
_BASELINE_MAX_DURATION_S = 2.5
_BASELINE_POLL_INTERVAL_S = 0.05


def _learn_screen_baseline(simulator, poll_fn,
                            stable_streak=_BASELINE_STABLE_STREAK,
                            max_duration=_BASELINE_MAX_DURATION_S,
                            poll_interval=_BASELINE_POLL_INTERVAL_S):
    """Return the set of _screen_signature values seen on the CURRENT
    screen while idling (no keypress sent) — sampled until the set stops
    growing, not for a fixed duration. See _BASELINE_STABLE_STREAK for why.

    This is what makes the nudge loop's screen-based evidence reliable
    despite C-Dogs' menus animating at rest: instead of trusting a single
    before/after sample (which would trip on the very next blink frame
    with no keypress involved at all), the loop first characterizes every
    value this screen's own idle animation cycles through, then treats
    only a signature OUTSIDE that known set as real evidence of a
    transition.

    Also calls poll_fn() on every iteration, exactly like every other wait
    in this drive — this window is not a spectator to the log output
    despite being focused on the screen: skipping that poll here would
    silently reopen the eviction race this drive exists to close (see
    _accumulate's and _POLL_INTERVAL_S's docstrings). poll_fn is threaded
    through for exactly that purpose, not because this function otherwise
    needs to know about HEAPSTAT/GFXSTAT — it's the caller's own
    accumulate-everything poll closure (see _drive_quickplay), so every
    stream the caller cares about keeps accumulating here too.
    """
    baseline = set()
    stale_streak = 0
    deadline = time.time() + max_duration
    while time.time() < deadline and stale_streak < stable_streak:
        poll_fn()
        sig = _screen_signature(simulator)
        if sig is not None:
            if sig in baseline:
                stale_streak += 1
            else:
                baseline.add(sig)
                stale_streak = 0
        time.sleep(poll_interval)
    if not baseline:
        # A live simulator should always yield at least one sample; only
        # hit if display_stats failed on every single attempt above
        # (e.g. a transient RPC hiccup for the whole window). One last
        # try so callers get a real (possibly singleton) set rather than
        # an empty one that would make everything look like a transition.
        sig = _screen_signature(simulator)
        if sig is not None:
            baseline.add(sig)
    return baseline


def _drive_quickplay(simulator, streams, settle_s=45, done=None):
    """Launch C-Dogs, drive the quick-play menu flow, and return every
    requested diagnostic stream once navigation has demonstrably worked.

    `streams` is a list of (name, parse_fn) pairs, e.g.
    [("HEAPSTAT", parse_heapstats), ("GFXSTAT", parse_gfxstats)]. Every
    poll of the simulator's output is parsed with EVERY parse_fn from a
    single get_output() call and accumulated into that stream's own
    running list (see _accumulate). Returns {name: [stats...]}.

    Used to be called once per report kind — once for HEAPSTAT (from
    test_heapstat_is_emitted_and_truthful) and once for GFXSTAT (from
    peak_gfx_total, itself called from three separate tests) — each
    paying for its own boot, ~22MB data copytree, and navigation drive
    (issue #14 originally shared just the drive logic between those two
    call sites; prereq-6 went further and merged the call sites
    themselves). HEAPSTAT and GFXSTAT are always emitted as a pair from
    the same picos_asset_load_tick() report (apps/cdogs/stubs.c calls
    picos_heap_report() immediately followed by picos_gfx_report(), same
    tag, same tick) — one regex matches "HEAPSTAT ...", the other
    "GFXSTAT ...", against identical polls of the same text — so driving
    them separately was always redoing the same boot/stage/navigate work
    twice for zero additional coverage. This is now called exactly once,
    by the cdogs_quickplay_stats fixture, requesting both streams
    together; see .superpowers/sdd/prereq-6-report.md.

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

    Drives with an adaptive nudge-and-check loop rather than a fixed
    3-Enter sequence on a timer: send one Enter, then poll for direct
    evidence it actually registered (a completion signal, or the screen
    itself changing — see _screen_signature) before sending the next,
    with a generous per-nudge timeout rather than a fixed sleep. A fixed
    0.8s-per-keypress delay (this drive's previous approach) assumes the
    simulator always processes a keypress and redraws within that window;
    under host contention it sometimes doesn't, so the next Enter would
    land before the previous one had taken effect and get eaten by
    whatever menu level the drive was actually still on. Nudging on
    observed evidence instead removes that race: no keypress is sent
    "blind". Extra nudges beyond the 3 actually needed are harmless — see
    the nudge loop's own comment for why resending Enter is safe from any
    of the three levels this drive can get stuck at — so this converges
    reliably even when several presses in a row don't land.

    done: optional predicate(state) -> bool, where state is the same
    {name: [stats...]} dict this function returns, checked every poll of
    the settle loop; polling stops as soon as it returns True (or
    settle_s elapses, whichever first). Defaults to "every requested
    stream has collected more than one report" — the signal that
    quick-play's post-navigation report actually landed for every stream,
    not just the boot-time one. Callers with a more specific completion
    signal (e.g. "the heap peak has cleared a threshold") can supply
    their own so the loop breaks the moment that particular condition is
    satisfied rather than always waiting on report count.
    """
    if done is None:
        done = lambda state: all(len(v) > 1 for v in state.values())  # noqa: E731

    # Accumulated across every poll for the rest of this drive, one set
    # of state per requested stream — see _accumulate's docstring for why
    # a single end-of-drive read of get_output() isn't safe against its
    # bounded tails.
    seen = {name: set() for name, _ in streams}
    stats = {name: [] for name, _ in streams}

    def poll():
        text = _combined_output(simulator)
        for name, parse_fn in streams:
            _accumulate(text, parse_fn, seen[name], stats[name])
        return text

    def missing_streams():
        return [name for name, _ in streams if not stats[name]]

    simulator.launch_app("cdogs")

    # Wait for the first report on every requested stream: a fast,
    # reliable signal that the instrumentation is wired up and C-Dogs has
    # started booting. NOT a signal that the main menu is ready for input
    # — empirically this fires during early graphics/font init
    # (picos_asset_load_tick's very first call always clears its tick
    # gate), well before the campaign manifest scan that follows it
    # finishes.
    deadline = time.time() + 60
    text = ""
    while time.time() < deadline:
        text = poll()
        if not missing_streams():
            break
        time.sleep(_POLL_INTERVAL_S)
    missing = missing_streams()
    assert not missing, (
        f"no {'/'.join(missing)} lines found in log:\n{text[-2000:]}"
    )

    # The real "ready for input" signal: cdogs_picos.c logs this literal
    # line via api->sys->log() (routed to real stdout the same way
    # HEAPSTAT/GFXSTAT are — see module docstring) immediately before
    # entering the menu's LoopRunnerRun(), i.e. the first point input is
    # actually consumed. Sending Enters before this landed is the failure
    # mode that broke this drive previously: the campaign manifest scan
    # after the boot-time report can still be in flight, so an early Enter
    # is silently dropped (the app isn't polling input yet), leaving one
    # too few keypresses actually registered and the flow stuck one menu
    # level short of loading anything. Waiting for this line explicitly
    # replaces what used to be an assumption baked into fixed sleeps.
    # Keeps polling every stream each iteration too — the campaign
    # manifest scan (and any reports it produces) can still be in flight
    # here.
    deadline = time.time() + 60
    while time.time() < deadline:
        text = poll()
        if MENU_READY_MARKER in text:
            break
        time.sleep(_POLL_INTERVAL_S)
    assert MENU_READY_MARKER in text, (
        f"'{MENU_READY_MARKER}' never appeared in the log — C-Dogs did not "
        f"reach an input-ready main menu:\n{text[-2000:]}"
    )

    # Adaptive nudge-and-check: send one Enter, then poll (both the log
    # output and a screen fingerprint) for direct evidence it registered
    # before sending the next, instead of trusting a fixed sleep to have
    # been long enough. Resending Enter is safe from any of the three menu
    # levels this drive can get stuck at (main menu, "Start:" submenu, or
    # the campaign list — e.g. if an Enter races LoopRunnerRun's input
    # poll and gets dropped, observed directly under host CPU contention
    # from other concurrent processes) because each of those levels'
    # default-selected item leads deeper into quick-play. A real
    # menu-layout drift, by contrast, still never produces a second
    # report no matter how many Enters are sent, and still fails below
    # with the same diagnostic.
    #
    # _EXPECTED_TRANSITIONS caps how many CONFIRMED transitions this loop
    # will chase with fresh Enters — exactly the 3 real levels (Start >
    # Campaign > first campaign in the list), not an arbitrarily larger
    # number. Earlier development of this drive sent Enters far more
    # liberally (nudging again on every timeout, uncapped) on the
    # assumption from the task brief that extra Enters are harmless once
    # the campaign has loaded; that held under light load, but under
    # sustained host contention a nudge's Enter can land *while the app is
    # already busy loading* rather than idle at the briefing screen — and
    # this simulator's ARM code runs under Unicorn CPU emulation (not
    # natively), so "busy loading" can legitimately last many seconds,
    # widening that unsafe window considerably. A keypress landing there
    # is not obviously safe: reproduced directly while developing this
    # drive, a run that sent dozens of extra Enters past the third real
    # transition went on to observe a screen that never changed again for
    # 400+ seconds and no second report — consistent with extra input
    # having been queued and then fired once the app became responsive,
    # overshooting past the state this drive needs to observe. Capping at
    # exactly the expected count removes that risk: once 3 transitions are
    # confirmed, this loop stops sending input entirely and only waits
    # (see the settle loop below) — see .superpowers/sdd/prereq-5-report.md.
    _EXPECTED_TRANSITIONS = 3
    _MAX_NUDGES = 15  # headroom for dropped presses before any of the 3 land
    _NUDGE_TIMEOUT_S = 5.0  # ceiling on one nudge's wait for evidence
    _NAV_TIMEOUT_S = 60.0  # backstop on the whole nudge phase

    nav_deadline = time.time() + _NAV_TIMEOUT_S
    baseline = _learn_screen_baseline(simulator, poll)
    confirmed_transitions = 0
    for _ in range(_MAX_NUDGES):
        if (done(stats) or confirmed_transitions >= _EXPECTED_TRANSITIONS
                or time.time() >= nav_deadline):
            break

        simulator.keypress("enter")

        step_deadline = min(time.time() + _NUDGE_TIMEOUT_S, nav_deadline)
        last_screen_check = 0.0
        transitioned = False
        while time.time() < step_deadline:
            # Keep polling every stream regardless of which evidence is
            # being waited on: the third Enter (loading a campaign) is
            # exactly when the dense "[TRAMP] fs_*" burst from
            # campaign/map/sprite I/O happens, and the whole point of
            # polling throughout instead of only at the edges is that a
            # burst entirely between two checks can still evict a report
            # no poll ever saw — see _POLL_INTERVAL_S.
            poll()
            if done(stats):
                break

            now = time.time()
            if now - last_screen_check >= _SCREEN_POLL_INTERVAL_S:
                last_screen_check = now
                sig = _screen_signature(simulator)
                # A signature outside this screen's own known idle-blink
                # set is direct evidence this Enter registered and the app
                # rendered something new (a menu transitioned, or a load
                # screen appeared) — move on rather than continuing to
                # wait out the rest of this nudge's timeout for no reason.
                if sig is not None and sig not in baseline:
                    transitioned = True
                    break

            time.sleep(_POLL_INTERVAL_S)

        if done(stats):
            break
        if transitioned:
            confirmed_transitions += 1
            if confirmed_transitions >= _EXPECTED_TRANSITIONS:
                # The third confirmed transition is the load-triggering
                # Enter itself — stop sending input and drop straight to
                # the settle loop below, which just watches for the
                # completion signal without touching the keyboard again.
                break
            # Learn the new screen's own idle-blink set before the next
            # nudge, so that comparison is against what *this* screen does
            # at rest, not the previous one's.
            baseline = _learn_screen_baseline(simulator, poll)
        # else: no out-of-baseline signature turned up within this nudge's
        # timeout — most likely this Enter was dropped (e.g. it raced the
        # app's own input poll under host contention) rather than the menu
        # genuinely not responding, so the baseline is still valid and the
        # next loop iteration just tries again.

    # One more generous poll window for the completion signal — no more
    # Enters sent here even if confirmed_transitions never reached
    # _EXPECTED_TRANSITIONS (e.g. nav_deadline or _MAX_NUDGES ran out
    # first): past this point, sending more input is exactly the
    # "extra Enter mid-load" risk _EXPECTED_TRANSITIONS exists to avoid,
    # and the caller's own assertions below already produce a clear
    # diagnostic if navigation genuinely never got this far. If
    # done(stats) is already true this is a no-op single poll, not an
    # unconditional wait.
    settle_deadline = time.time() + settle_s
    text = poll()
    while time.time() < settle_deadline and not done(stats):
        time.sleep(_POLL_INTERVAL_S)
        text = poll()

    missing = missing_streams()
    assert not missing, (
        f"no {'/'.join(missing)} lines found in log:\n{text[-2000:]}"
    )

    # Navigation sanity check, once per stream. Each downstream test's own
    # substantive assertions are the only gate between "quick-play loaded"
    # and a pass, and on their own a failure there just says the expected
    # data never showed up — which points straight at the instrumentation
    # (stubs.c / pic.c / picos_heap.h). But the far more likely real cause
    # is that the three `enter` presses above no longer land on Start >
    # Campaign > first campaign (e.g. the main menu or a submenu
    # gained/lost/reordered an item and the default selection shifted):
    # C-Dogs would then stay idle after the first, boot-scan-only report
    # and never produce a second one, since quick-play is what drives the
    # extra campaign/map/sprite file I/O that triggers it. Catch that case
    # here with a message that names the actual suspect.
    for name, _ in streams:
        assert len(stats[name]) > 1, (
            f"only one {name} line observed after adaptively driving "
            f"quick-play (up to {_MAX_NUDGES} Enters: Start > Campaign > first "
            "campaign in the list, each sent only after evidence the previous "
            "one registered) — the main menu layout likely drifted from what "
            "this drive expects (main menu Start/Options/Quit -> \"Start:\" "
            "submenu Campaign/Dogfight/Deathmatch/Join game/Back -> \"Select a "
            "campaign:\" list; every Enter here relies on landing on an "
            "already-default-selected top item at that level) rather than a "
            f"{'heap' if name == 'HEAPSTAT' else 'graphics'}-instrumentation bug"
        )

    return stats


def _quickplay_settled(state):
    """Completion predicate for the merged HEAPSTAT+GFXSTAT drive: true once
    every stream has more than the lone boot-time report AND the heap
    tracker's peak has cleared LOADED_PEAK_FLOOR_BYTES.

    This is the union of what the two drives needed separately before they
    were merged (prereq-6): test_heapstat_is_emitted_and_truthful used to
    pass _drive_quickplay a done= that stopped as soon as peak cleared the
    floor, while every GFXSTAT-consuming test relied on the plain default
    of "more than one report". Requiring both here means the settle loop
    only stops early once every condition any downstream test needs is
    already satisfied — never earlier, so no consuming test can observe a
    state that wouldn't already have made its own old solo drive stop.
    Falling short of this by the settle timeout is not fatal on its own:
    _drive_quickplay's own navigation-sanity assertions (len > 1 per
    stream) and each test's substantive assertions below still catch a
    real regression either way.
    """
    heap = state.get("HEAPSTAT", [])
    gfx = state.get("GFXSTAT", [])
    return (
        len(heap) > 1
        and len(gfx) > 1
        and any(s["peak"] > LOADED_PEAK_FLOOR_BYTES for s in heap)
    )


@pytest.fixture(scope="module")
def cdogs_quickplay_stats(cdogs_simulator):
    """Drive C-Dogs quick-play exactly ONCE for the whole module and
    return both diagnostic streams it produces.

    Only ~100 of 1683 PNGs are even attempted at the idle main menu (2
    succeed, the rest are skipped by the reserve guard before a campaign
    is loaded) — the real sprite (and heap) load happens on campaign
    entry, so a plain launch-and-wait would see almost nothing on either
    stream. See _drive_quickplay for the full navigation rationale.

    Returns {"HEAPSTAT": [...], "GFXSTAT": [...], "RENDERPRESENT": [...]} —
    see parse_heapstats / parse_gfxstats / parse_renderpresents for the
    shape of each entry. RENDERPRESENT was added for
    test_boot_loading_screen_is_not_blank; every other test below only
    reads HEAPSTAT/GFXSTAT. None of them re-drive the simulator.
    """
    return _drive_quickplay(
        cdogs_simulator,
        [("HEAPSTAT", parse_heapstats), ("GFXSTAT", parse_gfxstats),
         ("RENDERPRESENT", parse_renderpresents)],
        done=_quickplay_settled,
    )


def _peak_gfx_entry(gfx_stats):
    """Return the GFXSTAT line whose peak= field equals the overall
    observed high-water mark, from an already-collected list of
    parse_gfxstats dicts.

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
    element of gfx_stats instead of this function's return value.
    """
    return max(gfx_stats, key=lambda s: s["peak"])


def test_heapstat_is_emitted_and_truthful(cdogs_quickplay_stats):
    """The heap gauge reports both readings, and true >= watermark.

    Guards against regressing to the watermark-only gauge, which silently
    under-reported free memory and mis-tuned the reserve guard.

    Reads the HEAPSTAT stream from the module's single shared quick-play
    drive (see cdogs_quickplay_stats), which opens campaign/map/sprite
    data and reliably produces at least one more report after the heap
    has grown well past the boot-time sample (verified directly: the
    resulting peak is consistently well above LOADED_PEAK_FLOOR_BYTES).
    The idle main menu alone never re-triggers file I/O, so without that
    drive only the single boot-time report would ever arrive and the peak
    field would go unexercised.
    """
    stats = cdogs_quickplay_stats["HEAPSTAT"]

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
    # and never witness the heap growing past a boot-time-only sample. The
    # peak tracker is sampled on every _sbrk() growth, not just at report
    # time, so it must have observed the heap actually filling up during
    # asset loading, well beyond the ~488_000-byte boot-time figure,
    # regardless of sampling cadence. (This used to compare against the
    # utils.c IMG_LOAD_HEAP_RESERVE guard's 2.5 MiB trip point instead;
    # freeing memory elsewhere means the guard no longer trips at all, so
    # that comparison is gone — see LOADED_PEAK_FLOOR_BYTES above.)
    max_peak = max(s["peak"] for s in stats)
    assert max_peak > LOADED_PEAK_FLOOR_BYTES, (
        f"max observed peak ({max_peak}) never exceeded the loaded-state "
        f"floor ({LOADED_PEAK_FLOOR_BYTES}) — instrumentation did not "
        "witness the heap filling during asset loading (it may be "
        "sampling only the boot-time state)"
    )


def test_gfxstat_reports_resident_graphics(cdogs_quickplay_stats):
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
    peak = _peak_gfx_entry(cdogs_quickplay_stats["GFXSTAT"])

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


def test_textures_borrow_rather_than_duplicate(cdogs_quickplay_stats):
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
    peak = _peak_gfx_entry(cdogs_quickplay_stats["GFXSTAT"])

    # Measured on this simulator (deterministic across repeated runs — these
    # are fixed 320x240 window buffers created once during
    # GraphicsInitialize, independent of how many sprites load):
    #   5 owning textures * 320 * 240 * 2 bytes = 768_000
    # (Sub-project 2A converted these from ARGB8888 to RGB565; they were
    # 1_536_000 before.) These five buffers are window-sized and fixed in
    # number, so this figure is deterministic — it does NOT grow with the
    # number of sprites loaded. That is what lets the ceiling sit tight.
    # If a per-pic duplication path were reintroduced, tex would gain
    # roughly one more copy of `data` per pic; at the current data=310_376
    # that lands near 768_000 + 310_376 ~= 1_078_376. The ceiling below
    # leaves a small margin over the deterministic baseline and sits far
    # below that regression estimate.
    TEX_CEILING_BYTES = 810_000
    assert peak["tex"] < TEX_CEILING_BYTES, (
        f"tex holds {peak['tex']} bytes, expected under {TEX_CEILING_BYTES} "
        "— legitimate owning textures (grafx.c's window-sized render "
        "buffers) measured at 768_000 on this simulator; a figure "
        "meaningfully above that suggests a per-pic texture-duplication "
        "path was reintroduced somewhere"
    )
    assert peak["data"] > 0, "no pic data counted; accounting is broken"

    # Secondary gate testing what this test is actually named for: tex
    # does not duplicate per-pic data. bytes-per-pic (data/pics) used to
    # stand in for this, but it assumed a resident set dominated by tiny
    # font glyphs; freeing the duplicate texture copy relaxed the reserve
    # guard (utils.c's IMG_LOAD_HEAP_RESERVE) so real sprites now load too
    # (pics: 512 -> 705 measured here), which legitimately raises the
    # average — that ratio isn't comparable across a change that alters
    # which assets load, so it's gone rather than re-tuned.
    #
    # The invariant that actually matters has nothing to do with pics at
    # all: grafx.c's GraphicsInitialize creates exactly 5 owning buffers,
    # each 320x240 px, each either RGB565 (2 bytes/px) or ARGB8888 (4
    # bytes/px) depending on how many of them have been converted so far —
    # bounding tex to 768_000..1_536_000 bytes regardless of how many pics
    # are resident. A reintroduced per-pic duplication path would instead
    # grow tex roughly in step with `data`, which TEX_CEILING_BYTES above
    # already catches. FIXED_WINDOW_TEX_FLOOR_BYTES guards the other
    # direction: a collapse in the accounting (tex reading near 0) instead
    # of merely shrinking.
    #
    # Measured today: tex=921_600 (4 of the 5 buffers already RGB565,
    # 1 still ARGB8888). A follow-on change converts the framebuffer and
    # that last remaining buffer, taking tex to the exact floor, 768_000 —
    # a later task can tighten this assertion to that exact figure once
    # that lands; until then the floor sits with headroom below it so both
    # states pass.
    assert peak["pics"] > 0, "no pics counted; accounting is broken"
    FIXED_WINDOW_TEX_FLOOR_BYTES = 700_000
    assert peak["tex"] >= FIXED_WINDOW_TEX_FLOOR_BYTES, (
        f"tex holds only {peak['tex']} bytes, below the fixed-window-buffer "
        f"floor of {FIXED_WINDOW_TEX_FLOOR_BYTES} — that floor is derived "
        "purely from grafx.c's 5 fixed 320x240 buffers, independent of "
        "pics/data, so a figure below it means texture accounting "
        "collapsed rather than merely shrank"
    )


def test_owned_textures_are_16_bit(cdogs_quickplay_stats):
    """Shim-owned whole-screen textures hold 2 bytes per pixel, not 4.

    grafx.c's GraphicsInitialize creates five 320x240 buffers via
    SDL_CreateTexture (bkgTgt, bkg, screen, hud, brightnessOverlay).
    At 4 bytes per pixel that is 1_536_000 bytes — 29% of the 5MB app
    heap spent on fixed window buffers. RGB565 halves each of them.

    Asserted as a ceiling rather than an equality because the count of
    owning textures is a property of grafx.c, not of the shim, and a
    second window (Graphics.SecondWindow) would legitimately add more.
    The ceiling sits below the all-32-bit figure so a regression to
    4-byte pixels cannot pass.
    """
    peak = _peak_gfx_entry(cdogs_quickplay_stats["GFXSTAT"])

    ALL_32BIT_BYTES = 5 * 320 * 240 * 4  # 1_536_000
    assert peak["tex"] < ALL_32BIT_BYTES, (
        f"tex holds {peak['tex']} bytes, which is not below the "
        f"all-ARGB8888 figure of {ALL_32BIT_BYTES} — owned textures do "
        "not appear to have been converted to RGB565"
    )
    assert peak["tex"] > 0, "no owning textures counted; accounting is broken"


def test_render_targets_are_16_bit(cdogs_quickplay_stats):
    """Every shim-owned texture is RGB565, including the render target.

    Task 3 left bkgTgt (SDL_TEXTUREACCESS_TARGET) at ARGB8888 so
    get_target() could keep one pointer type while the framebuffer was
    still 32-bit. Once the framebuffer is RGB565 that exception is gone,
    and tex should sit at the all-16-bit figure rather than 153_600
    above it.
    """
    peak = _peak_gfx_entry(cdogs_quickplay_stats["GFXSTAT"])

    # 5 whole-screen 320x240 textures at 2 bytes per pixel. bkgTgt, the one
    # SDL_TEXTUREACCESS_TARGET texture, is the last to convert (Task 4).
    TASK3_INTERIM_BYTES = ALL_16BIT_TEX_BYTES + 320 * 240 * 2  # 921_600
    assert peak["tex"] < TASK3_INTERIM_BYTES, (
        f"tex holds {peak['tex']} bytes, at or above the Task 3 interim "
        f"figure of {TASK3_INTERIM_BYTES} — the render target (bkgTgt) "
        "still looks like ARGB8888"
    )
    assert peak["tex"] > 0, "no owning textures counted; accounting is broken"


def test_render_pipeline_saving(cdogs_quickplay_stats):
    """The five window textures sit at the RGB565 figure, from both sides.

    Bounded above and below on purpose. The ceiling catches `tex` growing —
    a revert to 4-byte pixels (~1_536_000) or a reintroduced per-pic
    duplication path (~1_078_376). The floor catches `tex` shrinking —
    buffers not allocated, allocated undersized, or dropped from the
    accounting. The floor matters because this is a memory-reduction
    change: an undercount would look like a further saving rather than a
    defect, so the figure has to be pinned from both sides to be
    trustworthy.

    Not covered here, because g_picos_pic_tex_bytes only counts textures
    created through SDL_CreateTexture: PicosRenderer.framebuf (307_200 ->
    153_600) and the deleted s_rgb565_buf staging buffer (153_600). Those
    307_200 further bytes are verified statically in the same-named task
    step, by reading the two calloc sites in picos_sdl_impl.c.
    """
    peak = _peak_gfx_entry(cdogs_quickplay_stats["GFXSTAT"])

    EXPECTED = ALL_16BIT_TEX_BYTES  # 768_000
    assert EXPECTED * 0.9 < peak["tex"] < EXPECTED * 1.1, (
        f"tex holds {peak['tex']} bytes; expected ~{EXPECTED} "
        f"(5 x 320 x 240 x 2). Roughly {EXPECTED * 2} would mean the "
        "buffers reverted to ARGB8888; a much larger figure would mean a "
        "per-pic texture-duplication path came back."
    )


# Floor for a single RenderPresent frame's `colored` pixel count (see
# test_boot_loading_screen_is_not_blank). A fully blank/black loading-screen
# frame reads colored=0 (the RGB565-zero-is-opaque-black regression this
# test exists to catch — reproduced directly while writing this test: every
# one of RenderPresent #1-#4 read colored=0/76800 with the SDL_CreateTexture
# fix reverted). A healthy first frame measured on this simulator reads
# colored=138/76800 (panel art + logo + "Loading graphics..." text). The
# floor sits comfortably above the blank figure (0) and comfortably below
# the healthy one (138), so it fails hard on a blank frame without being
# brittle to small pixel-count drift from font/logo asset changes.
BOOT_FRAME_COLORED_FLOOR = 40


def test_boot_loading_screen_is_not_blank(cdogs_quickplay_stats):
    """C-Dogs' boot loading screens actually draw content, not solid black.

    Regression test for the Critical finding in the RGB565-conversion final
    review: SDL_CreateTexture relied on calloc's zero fill for a fresh
    texture's "nothing drawn yet" state. That was correct for ARGB8888
    (0x00000000 is alpha=0, transparent) but wrong for RGB565, which has no
    alpha channel — 0x0000 is opaque black, not transparent, and isn't
    PICOS_RGB565_CKEY either. g->screen (grafx.c's window texture, created
    with SDL_BLENDMODE_BLEND) was never written before the first
    LoadingScreenDraw() call, so every one of C-Dogs' boot loading screens
    (LoadingScreenDraw, cdogs_picos.c) rendered as solid black — both on
    real hardware and in this simulator.

    Nothing else in this module would have caught this: the GFXSTAT/
    HEAPSTAT assertions above only see byte counts, not pixel content, and
    a check anchored to the main menu (post MENU_READY_MARKER) would pass
    regardless of this bug, because the menu loop redraws g->screen every
    single frame — only the loading screens that run BEFORE the first real
    draw are exposed to a stale/zeroed buffer.

    This does NOT poll the display_stats RPC the way the reviewer's manual
    verification did (see the module's RENDERPRESENT_RE comment for why):
    an independent RPC poll during boot turned out to be unreliable for
    this specific purpose — it can observe the PicOS launcher's own
    leftover screen content from before C-Dogs ever presented a single
    frame, which reads as "non-blank" no matter what C-Dogs itself does,
    producing a false pass. Confirmed directly: an RPC-polling version of
    this test, tried first, PASSED even with the SDL_CreateTexture fix
    reverted. Parsing the RENDERPRESENT stream instead — the shim's own
    pre-existing fprintf(stderr, ...) census of each of the first 8
    presented frames, computed synchronously from the exact buffer just
    presented — has no such gap. RenderPresent numbering starts fresh at
    process start and only C-Dogs' own boot sequence (font load, 4x
    LoadingScreenDraw, first main-menu frame) produces the first 8, so
    every entry here is unambiguously one of C-Dogs' own frames.

    Verified directly against both states of this fix while writing it:
    stashing just the SDL_CreateTexture fix and rebuilding reproduced
    colored=0/76800 for every one of RenderPresent #1-#4 (all 4 boot
    loading screens); restoring the fix reproduced colored=138/76800 on
    #1 — see final-review-fix-report.md for both raw pytest runs.
    """
    frames = cdogs_quickplay_stats.get("RENDERPRESENT", [])
    assert frames, (
        "no RenderPresent debug lines found in the log — either the "
        "instrumentation in picos_sdl_impl.c's SDL_RenderPresent was "
        "removed, or C-Dogs never presented a frame at all"
    )

    # Only the boot-time frames are relevant here — MainMenu's own frames
    # (drawn continuously once the menu is up, and eligible to be numbered
    # anywhere from #5 up to the #8 cap depending on exactly how many
    # LoadingScreenDraw calls preceded them) legitimately have real content
    # regardless of this bug, so including them would dilute (not corrupt,
    # since max() is used below and a blank max only comes from an
    # all-blank set — but still worth being precise) what this test is
    # actually checking. cdogs_picos.c calls LoadingScreenDraw exactly 4
    # times before "Entering main menu loop", so #1-#4 are guaranteed to
    # all be loading-screen frames.
    boot_frames = [f for f in frames if f["num"] <= 4]
    assert boot_frames, (
        f"no RenderPresent frames numbered <= 4 in {frames!r} — expected "
        "C-Dogs' 4 boot-time LoadingScreenDraw calls to have presented "
        "frames #1-#4"
    )

    max_colored = max(f["colored"] for f in boot_frames)
    assert max_colored > BOOT_FRAME_COLORED_FLOOR, (
        f"every one of C-Dogs' boot loading-screen frames "
        f"({boot_frames!r}) peaked at only {max_colored} colored pixels "
        f"(floor {BOOT_FRAME_COLORED_FLOOR}) — this is what a solid-black "
        "loading screen looks like, exactly the "
        "zeroed-RGB565-texture-reads-as-opaque-black regression this test "
        "guards against"
    )
