"""Object lifetimes under GC and file-handle misuse (audit §3.3, §3.4).

gc_test runs twice, once with the default collector and once with a very
aggressive incremental collector (stress.flag). Each Lua case is one pytest
id per mode. The fs handle-misuse cases run as separate inline apps because
today they can take the simulator down, which must not cost the other cases.

Known review bugs are strict xfails; Tasks 9, 10 and 11 remove the markers.
"""

import pytest

from helpers import case_params, known_bug, lua_case_names, run_lua_app, stage_lua_app

GC_CASES = lua_case_names("gc_test")

GRAPHICS_BUG = ("review: Graphics Critical — sprite/spritesheet/tilemap/animation "
                "keep raw image pointers with no GC anchor; Task 10")
SPRITE_LIST_BUG = ("review: Graphics Critical — the display list holds raw sprite "
                   "pointers, so an added sprite can be collected; Task 10")
PERFORM_BUG = ("review: Graphics Critical — performOnAllSprites passes 4-byte proxy "
               "userdata (and today never calls the callback); Task 10")
SAMPLE_BUG = ("review: Audio High — a sampleplayer keeps no reference to its "
              "sample; Task 11")
FS_HANDLE_BUG = ("review: Audio/storage Critical — fs handles are lightuserdata "
                 "with no liveness check (double fclose / use after close); Task 9")

GC_KNOWN_BUGS = {
    "sprite_new_keeps_image": GRAPHICS_BUG,
    "sprite_setImage_keeps_image": GRAPHICS_BUG,
    "spritesheet_keeps_image": GRAPHICS_BUG,
    "tilemap_keeps_tileset": GRAPHICS_BUG,
    "animation_loop_keeps_frames": GRAPHICS_BUG,
    "added_sprite_survives_gc": SPRITE_LIST_BUG,
    "performOnAllSprites_visits_real_sprites": PERFORM_BUG,
    "sampleplayer_setSample_keeps_sample": SAMPLE_BUG,
    "sampleplayer_new_keeps_sample": SAMPLE_BUG,
}


def _stress(sd):
    (sd / "apps" / "gc_test" / "stress.flag").write_text("1")


@pytest.fixture(scope="module")
def gc_runs(lua_suite):
    return {"normal": lua_suite("gc_test"),
            "stress": lua_suite("gc_test", setup=_stress)}


@pytest.mark.parametrize("mode", ["normal", "stress"])
@pytest.mark.parametrize("case", case_params(GC_CASES, GC_KNOWN_BUGS))
def test_gc_lifetime(gc_runs, mode, case):
    gc_runs[mode].check_case(case)


@pytest.mark.parametrize("mode", ["normal", "stress"])
def test_gc_suite_survives(gc_runs, mode):
    """The whole run completes and the simulator stays healthy, known bugs
    included (the cases avoid touching a dangling parent)."""
    run = gc_runs[mode]
    run.assert_clean_exit()
    assert sorted(run.cases) == sorted(GC_CASES), run.describe()
    assert f"GC:MODE {mode}" in [e["text"] for e in run.log], run.describe()


# ── gc_test under ASan: use the parent after the child is collected ─────────

# The lifetime cases above stop at the survival check so the release sim stays
# deterministic. Under ASan each case also runs alone in use_first mode: the
# parent is used after the collection, and a missing anchor is a
# heap-use-after-free that ASan reports (and aborts on) at the bridge call.
# Reasons name the ASan report each case produced (task-22-report.md has the
# stacks). performOnAllSprites is not a lifetime case (its callback never
# runs), so it is not repeated here.
def _gc_asan(bug, site):
    return f"{bug}; ASan: heap-use-after-free in {site} on the collected child"


GC_ASAN_KNOWN_BUGS = {
    "sprite_new_keeps_image": _gc_asan(GRAPHICS_BUG, "l_sprite_draw"),
    "sprite_setImage_keeps_image": _gc_asan(GRAPHICS_BUG, "l_sprite_draw"),
    "spritesheet_keeps_image": _gc_asan(GRAPHICS_BUG, "l_spritesheet_drawFrame"),
    "tilemap_keeps_tileset": _gc_asan(GRAPHICS_BUG, "tilemap_draw <- l_tilemap_draw"),
    "animation_loop_keeps_frames": _gc_asan(GRAPHICS_BUG, "l_animation_loop_draw"),
    "added_sprite_survives_gc": _gc_asan(SPRITE_LIST_BUG, "l_sprite_update"),
    "sampleplayer_setSample_keeps_sample": _gc_asan(
        SAMPLE_BUG, "sound_player_play (sim_audio.c) <- l_sound_sampleplayer_play"),
    "sampleplayer_new_keeps_sample": _gc_asan(
        SAMPLE_BUG, "sound_player_play (sim_audio.c) <- l_sound_sampleplayer_play"),
}


def _use_first(case):
    def setup(sd):
        app = sd / "apps" / "gc_test"
        (app / "use_first.flag").write_text("1")
        (app / "only.flag").write_text(case)
    return setup


@pytest.mark.asan_only
@pytest.mark.parametrize("case", [
    pytest.param(c, id=c, marks=[known_bug(r)])
    for c, r in GC_ASAN_KNOWN_BUGS.items()])
def test_gc_use_after_collect(lua_suite, case):
    run = lua_suite("gc_test", setup=_use_first(case))
    run.check_case(case)


# ── fs handle misuse: one inline app per case ───────────────────────────────

FS_EDGE = {
    "double_close": """
        local f = T.ok(fs.open(fs.appPath("dc.txt"), "w"), "open")
        fs.write(f, "x")
        fs.close(f)
        pcall(fs.close, f)          -- must be a no-op or a clean Lua error
    """,
    "read_after_close": """
        local path = fs.appPath("rac.txt")
        local w = T.ok(fs.open(path, "w"), "open w")
        fs.write(w, "hello")
        fs.close(w)
        local f = T.ok(fs.open(path, "r"), "open r")
        fs.close(f)
        local ok, got = pcall(fs.read, f, 10)
        T.ok(not ok or got == nil, "read on a closed handle returned data")
    """,
    "write_after_close": """
        local f = T.ok(fs.open(fs.appPath("wac.txt"), "w"), "open")
        fs.close(f)
        local ok, n = pcall(fs.write, f, "x")
        T.ok(not ok or not n or n <= 0, "write on a closed handle succeeded")
    """,
    "negative_read_rejected": """
        local path = fs.appPath("neg.txt")
        local w = T.ok(fs.open(path, "w"), "open w")
        fs.write(w, "hello")
        fs.close(w)
        local f = T.ok(fs.open(path, "r"), "open r")
        local ok = pcall(fs.read, f, -1)
        T.ok(not ok, "read with a negative length was accepted")
        -- An absurd length is clamped to what is left in the file rather
        -- than allocated up front (it used to be umm_malloc(len)).
        T.eq(fs.read(f, 0x7fffffff), "hello")
        T.eq(fs.read(f, 10), nil)
        fs.close(f)
    """,
    "foreign_userdata_rejected": """
        -- A full userdata of another type is not a file handle.
        local u = T.ok(picocalc.sys.qmiPsramAlloc(64), "qmi alloc")
        T.ok(not pcall(fs.read, u, 10), "read accepted a qmi buffer")
        T.ok(not pcall(fs.write, u, "x"), "write accepted a qmi buffer")
        T.ok(not pcall(fs.close, u), "close accepted a qmi buffer")
        T.eq(picocalc.sys.qmiPsramWrite(u, 0, "still mine"), 10)
    """,
    "gc_closes_dropped": """
        -- FF_FS_LOCK = 16: 16 handles dropped without close must be closed
        -- by the collector, or the next open fails.
        for round = 1, 3 do
            for i = 1, 16 do
                T.ok(fs.open(fs.appPath("gc" .. i .. ".txt"), "w"),
                     "round " .. round .. " open " .. i)
            end
            collectgarbage("collect")
            collectgarbage("collect")
        end
    """,
    "methods_and_close_var": """
        local path = fs.appPath("meth.txt")
        do
            local w <close> = T.ok(fs.open(path, "w"), "open w")
            T.eq(w:write("abc"), 3)
            T.eq(w:tell(), 3)
        end                                  -- __close closes it here
        local f = T.ok(fs.open(path, "r"), "open r")
        T.eq(f:read(2), "ab")
        T.ok(f:seek(0), "seek")
        T.eq(fs.read(f, 3), "abc")
        f:close()
        f:close()                            -- idempotent
        T.ok(not pcall(f.read, f, 1), "read after f:close() succeeded")
        T.ok(tostring(f):find("closed"), "tostring: " .. tostring(f))
        T.eq(getmetatable(f), false)         -- metatable (and __gc) hidden
        T.eq(f.__gc, nil)
    """,
}

FS_EDGE_TAIL = """
        -- The filesystem must still work afterwards.
        local g = T.ok(fs.open(fs.appPath("after.txt"), "w"), "open after misuse")
        fs.write(g, "ok")
        fs.close(g)
        T.eq(fs.readFile(fs.appPath("after.txt")), "ok")
"""


def _fs_edge_app(body):
    return ("local fs = picocalc.fs\n"
            "local T = picocalc.sys.loadlib('picotest')\n"
            "T.case('edge', function()\n" + body + FS_EDGE_TAIL + "end)\n"
            "T.done()\n")


# write_after_close is undefined behaviour on a freed FILE in the release
# simulator: it crashes, "succeeds" or fails from run to run. Only the
# sanitizer build (make simulator-asan) can check it deterministically. The
# sim's file handle is a heap wrapper around the FILE* (hal_sdcard.c), so the
# misuse is a use-after-free in instrumented code that ASan reports at the
# bridge call (a bare FILE* is only touched inside glibc, which ASan can't
# see: read_after_close used to pass under ASan).
def _fs_asan(stack):
    return f"{FS_HANDLE_BUG}; ASan: heap-use-after-free, {stack}"


FS_EDGE_MARKS = {
    # release: SIGSEGV (fclose through the freed handle)
    "double_close": [known_bug(_fs_asan(
        "hal_sdcard_close <- l_fs_close (lua_bridge_fs.c) on the freed handle"))],
    # release: SIGSEGV
    "read_after_close": [known_bug(_fs_asan(
        "hal_sdcard_read <- l_fs_read (lua_bridge_fs.c) on the freed handle"))],
    "write_after_close": [known_bug(_fs_asan(
        "hal_sdcard_write <- l_fs_write (lua_bridge_fs.c) on the freed handle")),
        pytest.mark.asan_only],
    # today: pcall(fs.read, f, -1) returns nil (umm_malloc(-1) fails) and an
    # absurd length returns nil instead of the data
    "negative_read_rejected": [known_bug(FS_HANDLE_BUG)],
    # today: the qmi buffer is used as a FIL (garbage read, then a free of
    # the buffer's memory as a file)
    "foreign_userdata_rejected": [known_bug(FS_HANDLE_BUG)],
    # today: lightuserdata has no __gc, so the 17th open fails
    "gc_closes_dropped": [known_bug(FS_HANDLE_BUG)],
    # today: lightuserdata has no methods
    "methods_and_close_var": [known_bug(FS_HANDLE_BUG)],
}


@pytest.mark.parametrize("name", [pytest.param(n, id=n, marks=FS_EDGE_MARKS[n])
                                  for n in FS_EDGE])
def test_fs_handle_misuse(simulator, name):
    stage_lua_app(simulator.sd_card_path, f"fsedge_{name}", _fs_edge_app(FS_EDGE[name]))
    run = run_lua_app(simulator, f"fsedge_{name}", timeout=15)
    run.assert_all_passed(["edge"])


LEAK_APP = """
local fs = picocalc.fs
local T = picocalc.sys.loadlib("picotest")
T.case("open_20", function()
    -- FF_FS_LOCK = 16 (the simulator models it): the first 16 must open;
    -- 17..20 fail on hardware too. All are left open at exit.
    local handles = {}
    for i = 1, 20 do
        handles[i] = fs.open(fs.appPath("leak" .. i .. ".txt"), "w")
        if i <= 16 then T.ok(handles[i], "open " .. i) end
    end
    _G.__leaked = handles   -- returned to the launcher without closing
end)
T.done()
"""


# today: nothing closes the leaked handles, so the second run gets none
@known_bug(FS_HANDLE_BUG)
def test_fs_handles_leaked_at_exit(simulator):
    """An app that exits with 20 open files (16 of them real: FF_FS_LOCK)
    doesn't stop the next one from opening 16."""
    stage_lua_app(simulator.sd_card_path, "fsleak", LEAK_APP)
    for i in range(3):
        run = run_lua_app(simulator, "fsleak", timeout=15)
        run.assert_all_passed(["open_20"])
