"""E2E tests for the Lua VM configuration.

CMake passed LUA_32BITS=1 / LUAI_MAXSTACK=500 for years, but upstream
luaconf.h hard-coded both, so the VM silently ran with 64-bit integers,
double floats and a million-slot stack. cmake/picos_lua.cmake now patches
luaconf.h to honour the shared config (LUAI_MAXSTACK raised to 1000 — 500
overflowed the minesweeper flood fill); these tests pin the result down
through the lua_runtime fixture app, which logs one "LR <NAME> <value>" line
per probe.
"""
import time

import pytest


def _lines(sim):
    out = []
    for line in sim.get_log_buffer()["lines"]:
        out.append(line if isinstance(line, str) else line.get("text", ""))
    return out


def _run_fixture(sim, timeout=30.0):
    sim.launch_app("lua_runtime")
    deadline = time.time() + timeout
    while time.time() < deadline:
        lines = _lines(sim)
        if any("LR DONE" in l for l in lines):
            results = {}
            for l in lines:
                idx = l.find("LR ")
                if idx < 0:
                    continue
                name, _, value = l[idx + 3:].partition(" ")
                results[name] = value
            return results
        time.sleep(0.25)
    pytest.fail("LR DONE not seen within %.0fs:\n%s"
                % (timeout, "\n".join(_lines(sim)[-40:])))


BINARY_REJECTED = "nil attempt to load a binary chunk (mode is 't')"


def test_lua_runtime_config(simulator):
    r = _run_fixture(simulator)

    # 32-bit integers that wrap, single-precision floats
    assert r["MAXINT"] == "2147483647"
    assert r["MININT"] == "-2147483648"
    assert r["WRAP"] == "true"
    assert r["FLOATTYPE"] == "float"
    assert r["HUGE"] == "true"                  # 1e39 overflows float32
    assert r["FMT01"] == "0.1000000015"         # float32 nearest to 0.1
    assert r["HEX"] == "050c5d1f"               # FNV step wraps mod 2^32
    assert r["JSONF"] == "[0.1,0.33333334] rt=true"

    # coroutine and utf8 are open; the OS-facing libs stay closed
    assert r["CORO"] == "6"
    assert r["UTF8"] == "4:2"
    assert r["SANDBOX"] == "nil,nil,nil,nil"

    # load accepts source text only
    assert r["LOADTEXT"] == "42"
    assert r["LOADBC"] == BINARY_REJECTED
    assert r["LOADBC_MODE_B"] == BINARY_REJECTED
    assert r["LOADBC_READER"] == BINARY_REJECTED
    assert r["LOADBC_HEADER"] == BINARY_REJECTED

    # LUAI_MAXSTACK=1000: runaway recursion is a catchable "stack overflow"
    # (a trivial frame costs ~1 slot, so the depth lands just under the cap)
    assert r["RECURSE"] == "ok=false overflow=true"
    assert 900 < int(r["DEPTH"]) < 1000

    # LUAI_MAXCCALLS=60: Lua -> C -> Lua recursion (nested string.gsub
    # callbacks, the pattern that overflowed the old 4 KB C stack on device)
    # runs 40 deep and then stops with a catchable "C stack overflow" before
    # the 64 KB Lua VM stack is exhausted. Upstream's 200 would let it run
    # ~197 deep, about 170 KB of C stack on hardware.
    assert r["GSUB40"] == "x"
    assert r["GSUBDEEP"] == "ok=false cstack=true"
    assert 40 < int(r["GSUBDEPTH"]) < 60
