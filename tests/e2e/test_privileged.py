"""Privileged system calls are gated by app.json requirements (review:
Lua Critical "qmiPsram*/applyUpdate/sysconfig open to every app", Network
Medium "wifi_pass readable", Audio/storage Medium "PIO PSRAM has no bounds").

- `picocalc.sysconfig` exists only with the "sysconfig" requirement, and even
  then `wifi_pass` is write-only.
- `sys.applyUpdate` exists only with "system-update" AND an OS app id
  (the updater/store), and asks for confirmation before flashing.
- `sys.pioPsramRead/Write` refuse the OS-reserved range (MP3 ring + video
  pool below PIO_PSRAM_APP_BASE) and anything past the chip.
- `sys.qmiPsram*` handles are bounds-checked userdata, not raw pointers.
- `fs.browse(start)` cannot open a directory the app may not read.

Each group is an inline picotest app staged onto one module simulator (the
simulator rescans /apps when a launch misses).
"""

import json
import time
from pathlib import Path

import pytest

from helpers import run_lua_app, stage_lua_app

APP_BASE = 288 * 1024  # PIO_PSRAM_APP_BASE (src/drivers/pio_psram.h)

PLAIN_APP = f"""
local T = picocalc.sys.loadlib("picotest")
local sys = picocalc.sys
local APP_BASE = {APP_BASE}

T.case("sysconfig_absent", function()
    T.eq(picocalc.sysconfig, nil, "picocalc.sysconfig without the requirement")
end)

T.case("applyUpdate_absent", function()
    T.eq(sys.applyUpdate, nil, "sys.applyUpdate without system-update")
end)

T.done()
"""

SYSCONFIG_APP = """
local T = picocalc.sys.loadlib("picotest")

T.case("sysconfig_present", function()
    T.ok(picocalc.sysconfig, "picocalc.sysconfig with the requirement")
end)

T.case("wifi_pass_not_readable", function()
    local sc = picocalc.sysconfig
    T.ok(sc.load(), "sysconfig.load()")
    T.eq(sc.get("wifi_ssid"), "HomeNet")
    T.eq(sc.get("wifi_pass"), nil, "wifi_pass after load")
end)

T.case("wifi_pass_write_only", function()
    local sc = picocalc.sysconfig
    sc.set("wifi_pass", "changed-by-app")
    T.eq(sc.get("wifi_pass"), nil, "wifi_pass after set")
    T.ok(sc.save(), "sysconfig.save()")
end)

T.case("other_keys_read_write", function()
    local sc = picocalc.sysconfig
    sc.set("t7_key", "v")
    T.eq(sc.get("t7_key"), "v")
end)

T.done()
"""

UPDATE_PRESENCE_APP = """
local T = picocalc.sys.loadlib("picotest")
T.case("applyUpdate_absent", function()
    T.eq(picocalc.sys.applyUpdate, nil,
         "applyUpdate for a non-OS app id with system-update")
end)
T.done()
"""

UPDATE_CONFIRM_APP = """
local T = picocalc.sys.loadlib("picotest")
T.case("applyUpdate_present", function()
    T.eq(type(picocalc.sys.applyUpdate), "function")
end)
T.case("applyUpdate_asks_first", function()
    picocalc.sys.log("T7:CONFIRM_NEXT")
    local ok, err = picocalc.sys.applyUpdate(APP_DIR .. "/fw.bin")
    T.eq(ok, false)
    T.eq(err, "cancelled")
end)
T.done()
"""


@pytest.fixture(scope="module")
def sim(sim_module_factory):
    def setup(sd):
        (sd / "system" / "config.json").write_text(json.dumps(
            {"wifi_ssid": "HomeNet", "wifi_pass": "hunter2"}))
    return sim_module_factory(setup=setup)


def _passed(run):
    run.assert_all_passed()


def _press_until_exit(sim, key, tries=10):
    """Press `key` until the app launched last exits (a modal is up)."""
    for _ in range(tries):
        time.sleep(0.3)
        sim.keypress(key)
        try:
            return sim.wait_for_exit(timeout=0.5)
        except TimeoutError:
            pass
    return sim.wait_for_exit(timeout=5)


def test_app_without_requirements_is_unprivileged(sim):
    stage_lua_app(sim.sd_card_path, "priv_plain", PLAIN_APP)
    _passed(run_lua_app(sim, "priv_plain"))


def test_sysconfig_requirement_hides_wifi_pass(sim):
    stage_lua_app(sim.sd_card_path, "priv_sysconfig", SYSCONFIG_APP,
                  requirements=["sysconfig"])
    _passed(run_lua_app(sim, "priv_sysconfig"))
    # set() is allowed (write-only), and save() persisted it.
    cfg = json.loads((Path(sim.sd_card_path) / "system" / "config.json").read_text())
    assert cfg.get("wifi_pass") == "changed-by-app", cfg


def test_system_update_needs_an_os_app_id(sim):
    stage_lua_app(sim.sd_card_path, "priv_upd_plain", UPDATE_PRESENCE_APP,
                  requirements=["system-update"], id="com.test.upd")
    _passed(run_lua_app(sim, "priv_upd_plain"))


def test_os_updater_without_requirement_has_no_apply_update(sim):
    stage_lua_app(sim.sd_card_path, "priv_upd_noreq", UPDATE_PRESENCE_APP,
                  id="com.picos.store")
    _passed(run_lua_app(sim, "priv_upd_noreq"))


def test_apply_update_asks_before_flashing(sim):
    """The OS updater id with system-update gets applyUpdate, and it shows a
    confirmation naming the file; Esc cancels without flashing."""
    stage_lua_app(sim.sd_card_path, "priv_upd_ok", UPDATE_CONFIRM_APP,
                  requirements=["system-update", "root-filesystem"],
                  id="com.picos.updater", files={"fw.bin": b"\0" * 4096})
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("priv_upd_ok")
    sim.wait_for_log("T7:CONFIRM_NEXT", timeout=15, since_seq=seq)
    outcome = _press_until_exit(sim, "esc")
    assert outcome.get("result") == "returned", outcome
    res = json.loads((Path(sim.sd_card_path) / "data" / "com.picos.updater" /
                      "test_results.json").read_text())
    cases = {c["name"]: c for c in res["cases"]}
    assert res["done"], res
    for name in ("applyUpdate_present", "applyUpdate_asks_first"):
        assert cases[name]["status"] == "PASS", cases[name]
