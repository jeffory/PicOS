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
# sanitizer build (Task 22) can check it deterministically.
FS_EDGE_MARKS = {
    "double_close": [known_bug(FS_HANDLE_BUG)],          # glibc aborts: double free
    "read_after_close": [known_bug(FS_HANDLE_BUG)],      # SIGSEGV
    "write_after_close": [known_bug(FS_HANDLE_BUG), pytest.mark.asan_only],
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
T.case("open_50", function()
    local handles = {}
    for i = 1, 50 do
        handles[i] = T.ok(fs.open(fs.appPath("leak" .. i .. ".txt"), "w"), "open " .. i)
    end
    _G.__leaked = handles   -- returned to the launcher without closing
end)
T.done()
"""


def test_fs_handles_leaked_at_exit(simulator):
    """An app that exits with 50 open files doesn't stop the next one from
    opening 50 (in the sim each leak is a host FILE*)."""
    stage_lua_app(simulator.sd_card_path, "fsleak", LEAK_APP)
    for i in range(2):
        run = run_lua_app(simulator, "fsleak", timeout=15)
        run.assert_all_passed(["open_50"])
