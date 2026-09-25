"""E2E tests for the Lua heap's small-object pools (src/os/small_alloc.c).

umm_malloc hands out 200-byte blocks and its 15-bit block index caps the
device heap at ~30,800 blocks, so every small Lua object cost a whole block:
20,000 small tables ({i, i*0.5, "s"..i}, three objects each) ran the device
out of memory (Task 30). The pools pack small objects into slabs carved from
umm. The small_alloc_stress fixture logs "SA <NAME> <value>" lines.

The default simulator heap is a counting allocator over the host malloc,
which has no block waste, so the device behaviour is tested under
--real-umm: the firmware's own umm_malloc on a device-sized arena.
"""
import re
import time

import pytest

LINE = re.compile(r"SA (\w+) ?(.*)$")


def _run(sim, timeout=90.0):
    """Run the fixture; return {NAME: value} once "SA DONE" is logged."""
    sim.launch_app("small_alloc_stress")
    deadline = time.time() + timeout
    lines = []
    while time.time() < deadline:
        lines = [l if isinstance(l, str) else l.get("text", "")
                 for l in sim.get_log_buffer()["lines"]]
        if any("SA DONE" in l for l in lines):
            out = {}
            for l in lines:
                m = LINE.search(l)
                if m:
                    out[m.group(1)] = m.group(2)
            return out
        if not sim.is_alive():
            break
        time.sleep(0.25)
    pytest.fail("SA DONE not seen within %.0fs:\n%s"
                % (timeout, "\n".join(lines[-40:])))


def _fields(value):
    return {k: v for k, v in (kv.split("=", 1) for kv in value.split())}


def _assert_phases(r):
    for phase in ("LIVE20K", "CHURN100K", "EMPTY20K", "GROW"):
        assert r.get(phase) == "ok", f"{phase}: {r.get(phase)}"


def test_small_objects_counting_heap(simulator):
    """The default simulator heap: every phase completes and the pools are
    in use (getMemInfo reports them)."""
    r = _run(simulator)
    _assert_phases(r)
    live = _fields(r["MEM_LIVE20K"])
    assert int(live["slabs"]) > 0, live
    assert int(live["objects"]) >= 60000, live   # 20k tables + arrays + strings


def test_small_objects_real_umm(sim_factory, test_sd_card):
    """On the device's own umm (200-byte blocks, 15-bit indices) 20,000
    small tables and 100,000 churned tables/strings fit. Without the pools
    LIVE20K runs out of memory, as it did on hardware."""
    sim = sim_factory(test_sd_card, extra_args=["--real-umm"])
    r = _run(sim)
    _assert_phases(r)

    start = _fields(r["MEM_START"])
    live = _fields(r["MEM_LIVE20K"])
    end = _fields(r["MEM_END"])
    assert int(live["slabs"]) > 0, live
    # More live objects than the heap has umm blocks (30,801 of 200 bytes):
    # only possible when several share a block.
    assert int(live["objects"]) > 30801, live
    # Slabs go back to umm as they empty: after a full collect the heap's
    # free bytes are within a few slabs of where they started.
    assert int(r["RETURNED_KB"]) < 64, r["RETURNED_KB"]
    assert int(end["slabs"]) < int(live["slabs"]) // 10, (live, end)


@pytest.mark.parametrize("extra_args", [[], ["--real-umm"]],
                         ids=["counting", "real_umm"])
def test_heap_returned_after_exit(sim_factory, test_sd_card, extra_args):
    """Every slab, the pool bookkeeping and every block routed through the
    pools go back to umm when the Lua state closes, so an app leaves nothing
    pinned in the heap (C-Dogs needs one ~5.9 MB block after a Lua app has
    run): a second run starts from exactly the heap the first one did,
    largest block included, and ends with the same free heap."""
    sim = sim_factory(test_sd_card, extra_args=extra_args)
    starts, free_after = [], []
    for _ in range(2):
        r = _run(sim)
        _assert_phases(r)
        starts.append(_fields(r["MEM_START"]))
        sim.wait_for_exit(timeout=15)
        free_after.append(sim.call("get_heap_info")["lua_heap_free_kb"])
        sim.clear_log()
    assert free_after[0] == free_after[1], f"heap not returned: {free_after}"
    for key in ("free", "largest"):
        assert starts[0][key] == starts[1][key], (key, starts)
