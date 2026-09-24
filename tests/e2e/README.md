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

### Sanitizer builds

```bash
make simulator-asan      # ASan + UBSan, build_sim_asan/ (clang; SIM_SAN_CC to override)
PICOS_SIM_BINARY=build_sim_asan/picos_simulator SDL_VIDEODRIVER=dummy pytest tests/e2e -n auto
make simulator-tsan      # TSan, build_sim_tsan/ (informational, see below)
```

`PICOS_SIM_BINARY` (or `--simulator-path`) picks the binary. The harness asks it
`--build-info` and runs the `asan_only` tests only against an ASan build; they
skip (allow-listed) otherwise. Every simulator gets `ASAN_OPTIONS`,
`UBSAN_OPTIONS` and `TSAN_OPTIONS` from `picos_simulator.SANITIZER_ENV` (any
already set in the environment win): ASan and UBSan reports are fatal, leak
checking is off (options you set yourself are merged in per key and win).
Set `PICOS_SIM_EXPECT_SANITIZE=address` (the ASan CI leg does) to make the run
refuse to start unless `--build-info` confirms the sanitizer; if the probe
fails without it, `asan_only` skips are no longer allow-listed. Install
`llvm-symbolizer` (or set `ASAN_SYMBOLIZER_PATH`) for symbolised stacks. The
sanitizer build leaves SIGSEGV/SIGABRT to the sanitizer, so the
evidence is the report on stderr rather than the sim's crash log. stderr is
drained from the moment the process starts, and the report is captured apart
from the 2000-line tail (capped at 600 lines), so neither report volume nor
later output can hide it or back up the pipe.

TSan reports on nearly every test, from the sim's own threads: the RPC socket
thread reads the framebuffer, launcher and keyboard state that the main thread
owns, and the Core 1 thread reads audio and lifecycle globals the main thread
writes, all unlocked (`specs/test-audit-2026-09-24.md` §2.6). So the TSan leg is
nightly and informational; `tests/e2e/tsan.supp` suppresses third-party
(libdbus) reports only.

### Firmware network stack (`firmware_net`)

```bash
make simulator-net       # build_sim_net/ (also simulator-net-asan, simulator-net-tsan)
PICOS_SIM_BINARY=build_sim_net/picos_simulator SDL_VIDEODRIVER=dummy \
    pytest tests/e2e/test_network_firmware.py -n auto
```

`make simulator-net` runs the firmware's own `src/drivers/wifi.c`, `http.c` and
`tcp.c` on Mongoose/POSIX (TLS off) instead of the sim's libcurl layer, with
Core 0 and Core 1 as two host threads (`simulator/net/`). The `firmware_net`
tests (`test_network_firmware.py`) run only against it (`--build-info` says
`firmware_net=1`) and skip, allow-listed, elsewhere;
`PICOS_SIM_EXPECT_FIRMWARE_NET=1` makes the run refuse to start without it.
Each case runs in its own simulator against local servers
(`net_servers.py`: HTTP on 127.0.0.1 with `/ok`, `/big`, `/chunked`, `/close`,
`/drip`, `/hang`, `/reset`, `/echo`; a TCP echo/flood server; a black-hole
port). The app `net_fw/main.lua` is staged per test and runs the one case
named in `/data/com.test.net_fw/servers.json`, so a case that crashes the
simulator fails alone. The code review's Core 0/Core 1 close races are
strict xfails there. The whole E2E suite also passes against this build.

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
├── native/              source + Makefile of the native fixture (apps/native_api_probe/main.elf)
├── elfgen.py            struct.pack ELF32 builder, valid or malformed, for the loader tests
├── fixtures/fonts/      golden PNGs
└── skip_allowlist.txt   tests that may skip; any other skip fails the run
```

## Native apps

`test_native.py` launches the committed `apps/hello_c/main.elf` (smoke) and
`apps/native_api_probe`, which logs the `PicoCalcAPI` layout it was compiled
against next to what the simulator handed it. Expected values come from the
sources: `g_api.version` from `src/main.c`, the layout from `src/os/os.h`
(parsed by `tools/check_native_abi.py`). The probe ELF is committed, so this
job needs no ARM toolchain; after changing `sdk/native/os.h`, rebuild it with
`make -C tests/e2e/native` and commit it. CI's `native-sdk` job (build-sim.yml)
compiles `sdk/native/main.c` and the probe and runs
`tools/check_native_abi.py --cc arm-none-eabi-gcc`, which compares the three
ABI copies (`src/os/os.h`, `sdk/native/os.h`, the trampolines' version write).

`test_native_malformed.py` stages one `elfgen.build_elf(...)` image per
malformation (`specs/test-audit-2026-09-24.md` §3.5 plus the other
`elf_plan` refusals) as `/apps/badelf_<case>/main.elf` and asserts the launch
ends `load_failed` with the exact `[UNICORN] ELF rejected: <reason>` on
stderr, nothing ran, the SD card is unchanged and the simulator still answers.
The reason goes to stderr only (not the log buffer or `/system/error.log`,
which the firmware loader writes).

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

After every test the harness checks the simulators the test used: `simulator`
and any other fixture value that is a simulator, plus every simulator started
through `sim_factory` (registered on the test) or `sim_module_factory`
(registered on the module). The process must still be running, there must be
no crash log, and stderr must not contain a sanitizer report. A failing test
gets the simulator's stdout/stderr tails, log buffer, `/system/error.log` and a
screenshot attached (terminal sections and pytest-html extras; files under
`--artifacts-dir`).

`lua_suite` stops its simulator before its tests run, so it is not in that
hook. Its health goes into `LuaRun.problems` instead: if the simulator dies
mid-run, `run_lua_app` returns outcome `simulator_died` with the crash log,
exit status, sanitizer lines and stderr tail as problems, and every case id
(`check_case`, `assert_*`) fails with that evidence.

## Quarantined flaky tests

`@pytest.mark.flaky(reason=...)` quarantines a known flake: when it fails, the
failure is reported as an xfail with a `quarantined flake: <reason>` reason and
listed in its own "quarantined flaky tests that failed" summary section, and the
run does not fail. Nothing is retried. A quarantined test whose simulator
crashed, wrote a crash log or reported a sanitizer error is **not** forgiven: it
fails the run like any other. Keep crash assertions out of flaky tests (see
`test_cdogs_memory.py::test_gameplay_drive_did_not_crash`). Quarantine is a
stopgap: each marker names the cause and the fix.

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
  pixel values prove simulator output. The exception is `make simulator-net`,
  which runs the firmware's `wifi.c`/`http.c`/`tcp.c` (without TLS).
- `get_heap_info` in the simulator counts the live `umm_*`/Lua bytes (a
  counting allocator over malloc: 8 MB minus live). The largest free block is
  approximated by the free total and fragmentation is always 0, so
  fragmentation and `min_psram_kb` behaviour are still device-only.
  `test_heap_metrics_live` proves the metric moves; the leak tests check it
  first (`require_heap_metrics_live`).
- `test_cdogs_memory.py` needs a built checkout of jeffory/picos-cdogs
  (`PICOS_CDOGS_DIR`, default `~/Projects/picos-cdogs`); it is allow-listed to skip.
