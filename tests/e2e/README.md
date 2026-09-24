# PicOS E2E tests

pytest suite that drives the SDL2 + Unicorn simulator (`build_sim/picos_simulator`)
over its JSON-RPC control channel.

## Running

```bash
pip install -r tests/e2e/requirements.txt
make simulator

SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto          # whole suite
pytest tests/e2e/test_system.py -v                       # one module
pytest tests/e2e --show-window                           # watch the simulator
pytest tests/e2e --update-baselines                      # rewrite golden PNGs
pytest tests/e2e --html=reports/e2e.html --self-contained-html \
                 --artifacts-dir reports/failures        # CI-style reports
```

Paths are absolute, so any working directory inside the repo works. The config
is the repo-level `pytest.ini`: 60 s per-test timeout (`pytest-timeout`), strict
xfail, and markers `slow`, `hardware`, `native`, `asan_only`, `flaky`, `sd`.

## Layout

```
tests/e2e/
├── conftest.py          fixtures and hooks (simulator, SD card, health check, skip allow-list)
├── helpers.py           SD staging, the Lua test-kit runner, golden compare, heap-metric check
├── picos_simulator.py   JSON-RPC client and process wrapper
├── lib/picotest.lua     Lua test kit, staged to /system/lib/picotest.lua
├── apps/<name>/         fixture apps (app.json + main.lua), staged onto every SD card
├── fixtures/fonts/      golden PNGs
└── skip_allowlist.txt   tests that may skip; any other skip fails the run
```

## Fixtures

- `simulator`: a fresh simulator per test, in `--test-mode` (Lua error screens
  return at once and their text goes to the log's `err` source), with no UNIX
  socket and a crash log in the test's tmp dir.
- `test_sd_card`: the per-test SD card. It is built from a manifest, not a copy
  of `simulator/assets/sd_card`: `apps/hello`, every `tests/e2e/apps/*` fixture,
  and `system/lib/*.lua` plus `picotest.lua`. Stage more with
  `@pytest.mark.sd(extra=["apps/foo", ("path/in/repo", "sd/dest")])`.
- `sim_factory`: start additional simulators (`sim_factory(sd_path, **kwargs)`).
- `lua_suite`: run a test-kit app once in its own simulator and return its
  cases (use from a module-scoped fixture).

After every test the harness checks each simulator the test used: the process
must still be running, there must be no crash log, and stderr must not contain
a sanitizer report. A failing test gets the simulator's stdout/stderr tails, log
buffer, `/system/error.log` and a screenshot attached (terminal sections and
pytest-html extras; files under `--artifacts-dir`).

## Synchronising with the simulator

Prefer these over `time.sleep`:

| Need | Call |
|---|---|
| app printed X | `sim.wait_for_log(regex, timeout, since_seq=, src=)` |
| app finished | `sim.launch_app(name)` then `sim.wait_for_exit(timeout)` → `{found, result, error, runtime_ms, launch_id}` |
| N frames presented | `sim.wait_frames(n)` |
| injected key consumed | `seq = sim.keypress(k)["input_seq"]; sim.wait_input_consumed(seq)` |
| app staged after boot | `stage_lua_app(sd, name, code, requirements=[...])` then `launch_app` (the sim rescans on a miss) |

## Writing a Lua test app (picotest)

```lua
local T = picocalc.sys.loadlib("picotest")

T.case("adds", function()
    T.eq(1 + 1, 2)
end)
T.case("needs network", function()
    T.skip("not in the simulator")   -- fails the run unless allow-listed
end)
T.done()
```

Checks: `T.eq(got, want, msg)`, `T.ok(v, msg)`, `T.fail(msg)`,
`T.raises(fn, pattern)`, `T.skip(reason)`. Each case logs
`[T] CASE <name> PASS|FAIL|SKIP <detail>`; `T.done()` logs
`[T] DONE pass=N fail=M skip=K`. Results are also written to
`/data/<APP_ID>/test_results.json` after every case. The kit restores
`APP_ID`/`APP_DIR`/`APP_NAME`/`APP_REQUIREMENTS` after each case.

On the Python side:

```python
from helpers import lua_case_names

CASES = lua_case_names("my_test")          # T.case names, read statically

@pytest.fixture(scope="module")
def my_run(lua_suite):
    return lua_suite("my_test")

@pytest.mark.parametrize("case", CASES)
def test_my_case(my_run, case):
    my_run.check_case(case)

def test_my_suite_complete(my_run):
    my_run.assert_all_passed(CASES)
```

For a case that depends on a known, unfixed bug, mark the pytest id
`xfail(strict=True, reason="review: <row>")`: it must fail today, and the fix
turns it into an XPASS, which fails the run until the marker is removed.

## Golden images

A missing golden fails the test. Create or refresh goldens with
`--update-baselines` and review the diff before committing them.

## Known limits

- The simulator runs its own display, audio, keyboard and network code, not
  `src/drivers/*` (see `specs/test-audit-2026-09-24.md` §2.7), so goldens and
  pixel values prove simulator output.
- `get_heap_info` is constant in the simulator; the leak tests are strict xfails
  until the heap metrics are real.
- `test_cdogs_memory.py` needs a built checkout of jeffory/picos-cdogs
  (`PICOS_CDOGS_DIR`, default `~/Projects/picos-cdogs`); it is allow-listed to skip.
