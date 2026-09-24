"""The Lua count hook and its service pass (review: Lua Medium "The debug hook
fires every 256 opcodes ... The sound poll pcalls from the hook and never pops
the error").

- A failing sound callback's error is popped. lua_bridge_service also runs
  from sys.sleep, whose C frame lives for the whole sleep: before the fix
  every error stayed on that stack until lua_rawgeti wrote past it (ASan:
  heap-buffer-overflow in finishrawget after ~1000 callbacks).
"""

from pathlib import Path

from helpers import stage_lua_app, write_wav


def test_failing_sound_callbacks_do_not_pile_up_during_sleep(simulator):
    """A looping fileplayer whose loop callback always raises fires ~100
    callbacks a second during one long sys.sleep; more than LUAI_MAXSTACK
    (1000) of them must neither overflow the stack nor stop firing."""
    code = """
local pc = picocalc
local n = 0
local fp = pc.sound.fileplayer()
assert(fp:load(APP_DIR .. "/tick.wav"), "load tick.wav")
fp:setLoopRange(0, 0)
fp:setLoopCallback(function() n = n + 1; error("loop callback failed") end)
fp:play()
pc.sys.sleep(12500)
local m = n
pc.sys.sleep(1000)
fp:stop()
pc.sys.log("SOUND_CALLS " .. m .. " " .. (n - m))
"""
    stage_lua_app(simulator.sd_card_path, "hook_sound_err", code)
    write_wav(Path(simulator.sd_card_path) / "apps" / "hook_sound_err" / "tick.wav",
              seconds=0.01)
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("hook_sound_err")
    outcome = simulator.wait_for_exit(timeout=40)
    assert outcome.get("result") == "returned", outcome
    line = simulator.wait_for_log(r"^SOUND_CALLS ", timeout=5, since_seq=seq)
    total, last = (int(v) for v in line.split()[1:3])
    assert total > 1000, f"only {total} loop callbacks in 12.5 s: {line}"
    assert last > 0, f"loop callbacks stopped firing: {line}"
