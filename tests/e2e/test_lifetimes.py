"""Object lifetimes under GC and file-handle misuse (audit §3.3, §3.4).

gc_test runs twice, once with the default collector and once with a very
aggressive incremental collector (stress.flag). Each Lua case is one pytest
id per mode. The fs handle-misuse cases run as separate inline apps because
a regression would take the simulator down, which must not cost the other
cases.

Known review bugs are strict xfails; Tasks 10 and 11 remove the markers
(Task 9 removed the fs ones).
"""

import pytest

from helpers import case_params, lua_case_names, run_lua_app, stage_lua_app

GC_CASES = lua_case_names("gc_test")

SAMPLE_BUG = ("review: Audio High — a sampleplayer keeps no reference to its "
              "sample; Task 11")

GC_KNOWN_BUGS = {}


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
# stacks). performOnAllSprites is not a lifetime case, so it is not repeated
# here.
def _gc_asan(bug, site):
    return f"{bug}; ASan: heap-use-after-free in {site} on the collected child"


GC_ASAN_KNOWN_BUGS = {}


def _use_first(case):
    def setup(sd):
        app = sd / "apps" / "gc_test"
        (app / "use_first.flag").write_text("1")
        (app / "only.flag").write_text(case)
    return setup


# Every lifetime case runs here; the ones whose fix has landed must pass.
GC_ASAN_CASES = [
    "sprite_new_keeps_image", "sprite_setImage_keeps_image",
    "spritesheet_keeps_image", "tilemap_keeps_tileset",
    "animation_loop_keeps_frames", "added_sprite_survives_gc",
    "sampleplayer_setSample_keeps_sample", "sampleplayer_new_keeps_sample",
]


@pytest.mark.asan_only
@pytest.mark.parametrize("case", case_params(GC_ASAN_CASES, GC_ASAN_KNOWN_BUGS))
def test_gc_use_after_collect(lua_suite, case):
    run = lua_suite("gc_test", setup=_use_first(case))
    run.check_case(case)


# ── fs handle misuse: one inline app per case ───────────────────────────────

FS_EDGE = {
    "double_close": """
        local f = T.ok(fs.open(fs.appPath("dc.txt"), "w"), "open")
        fs.write(f, "x")
        fs.close(f)
        fs.close(f)                 -- a second close is a no-op, not an error
    """,
    "read_after_close": """
        local path = fs.appPath("rac.txt")
        local w = T.ok(fs.open(path, "w"), "open w")
        fs.write(w, "hello")
        fs.close(w)
        local f = T.ok(fs.open(path, "r"), "open r")
        fs.close(f)
        T.raises(function() fs.read(f, 10) end, "closed file")
    """,
    "write_after_close": """
        local f = T.ok(fs.open(fs.appPath("wac.txt"), "w"), "open")
        fs.close(f)
        T.raises(function() fs.write(f, "x") end, "closed file")
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
    "readfile_oom_does_not_leak": """
        -- readFile of a file that needs more memory than is free must fail
        -- cleanly: the bytes read so far belong to Lua and are collected
        -- (it used to umm_malloc a copy and leak it when the push failed).
        local mem = picocalc.sys.getMemInfo
        collectgarbage("collect"); collectgarbage("collect")
        local size = math.floor(mem().psram_free * 0.6)
        local path = fs.appPath("big.bin")
        local w = T.ok(fs.open(path, "w"), "open w")
        local chunk = string.rep("x", 65536)
        local left = size
        while left > 0 do
            local n = math.min(left, #chunk)
            T.eq(fs.write(w, n == #chunk and chunk or chunk:sub(1, n)), n)
            left = left - n
        end
        fs.close(w)
        chunk = nil
        collectgarbage("collect"); collectgarbage("collect")
        local base = mem().psram_free
        local ok = pcall(fs.readFile, path)   -- needs ~2x size: out of memory
        collectgarbage("collect"); collectgarbage("collect")
        local lost = base - mem().psram_free
        T.ok(lost < 65536, ("readFile leaked %d of %d bytes (ok=%s)")
             :format(lost, size, tostring(ok)))
        fs.delete(path)
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


# Handles are full userdata (lua_bridge_fs.c): a closed or foreign handle is
# a Lua error, never a FIL, so every case is deterministic in both the release
# and the ASan simulator.
@pytest.mark.parametrize("name", list(FS_EDGE))
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


def test_fs_handles_leaked_at_exit(simulator):
    """An app that exits with 20 open files (16 of them real: FF_FS_LOCK)
    doesn't stop the next one from opening 16."""
    stage_lua_app(simulator.sd_card_path, "fsleak", LEAK_APP)
    for i in range(3):
        run = run_lua_app(simulator, "fsleak", timeout=15)
        run.assert_all_passed(["open_20"])
