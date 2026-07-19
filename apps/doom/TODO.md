# PicOS Audio / Memory / Performance Audit — TODO

Plan source: `~/.claude/plans/can-you-review-the-parallel-thunder.md`

## Status update (2026-07-18, evening session)

Both Doom blockers root-caused and fixed:

### 1. Launch crash — QMI PSRAM overclock corruption (FIXED)

The "first-frame hang" / hardfault-at-launch was **not** audio, memory, or
app-side at all. Apps requesting `system_clock_khz: 300000` (doom, gbc,
tic-80, c64, dos86) scaled the QMI CS1 PSRAM SCK with sysclk because
`launcher_apply_clock()` never retuned QMI timing — 50 MHz (validated, reset
CLKDIV=4 @ 200 MHz) became 75 MHz, out of spec. PSRAM *reads glitched
transiently*: instruction fetches of PSRAM-resident app code returned
garbage (PSRAM content itself stayed correct — verified with the CODEWATCH
scanner), producing hardfaults with CFSR=0/SFSR=0, impossible IPSR values,
and garbage at PC. Doom died in 3-22 s at 300 MHz; ran 1280+ clean scan
passes at 200 MHz.

Fix: `psram_qmi_apply_timing()` in `launcher.c` rescales
`qmi_hw->m[1].timing` CLKDIV to keep SCK ≤ 50 MHz across clock changes
(applied before with worst-case and after with exact sysclk).

Diagnostics added while chasing this (all in tree, worth keeping):
- Hardfault handler: SFSR/SFAR/DFSR, live IPSR, stacked xPSR, instr16@PC,
  ELF-relative PC/LR via `g_native_code_base/...` loader globals;
  crashlog scratch packing extended to persist them.
- `[NATIVE] Image base %p = ELF vaddr 0x%x` log line → symbolicate crashes
  with `arm-none-eabi-addr2line -e <unstripped>.elf <PC-rel>`.
- CODEWATCH: Core-1 rotating scan of the app's read-only image vs a
  load-time snapshot (uncached alias, transient-vs-persistent
  discrimination, repair-on-detect). Diagnostic only — consider removing
  or gating once things are stable.

### 2. Audio completely broken — FEATURE_SOUND never compiled in (FIXED)

`doomfeatures.h` (as vendored) contains only a commented-out
`#undef FEATURE_SOUND` and nothing ever defined it, so `i_picos_sound.c`
compiled to an empty TU, `sound_modules[]` was empty, and Doom ran silent
with zero errors. Fixed:
- `-DFEATURE_SOUND` added to `apps/doom/Makefile`
- `i_sound.c` (submodule): SDL_mixer include now behind `USE_SDL_MIXER`

On-hardware validation results (Phase-0 instrumentation):
- **SFX mixing runs at full rate** — [DOOM-AUDIO] shows ~156 calls /
  22 050 samples per 2 s window with zero gaps while only SFX mix.
- **OPL music synthesis cannot keep up**: Nuked-OPL3 from serial-mode
  50 MHz PSRAM sustains only ~6.5 k samples/s (11 025 needed). With music
  playing the Core 1 worker takes 130-220 ms per call, underruns
  continuously, and drags SFX down with it. Two additional aggravators
  were also fixed: `-DOPL_CAPTURE` auto-started SD writes on the Core 1
  audio path, and `MUS_DEBUG_VOICE` printf'd per voice event / per render
  (both now off by default with warnings in Makefile / mus_player.c).
- Interim: `-nomusic` added to argv in dg_picos.c — SFX-only audio is
  clean. Music needs QMI quad-mode PSRAM (~4× bandwidth) or a lighter
  OPL core; see below.

## Current state of the working tree

- `src/main.c` — Phase-0 audio stats printer ([AUDIO] every ~2 s while a
  native audio callback is registered); hardfault diagnostics; CODEWATCH
  Core-1 scanner.
- `src/os/native_loader.c` — image-placement globals for symbolication;
  CODEWATCH snapshot arm/cleanup.
- `src/os/launcher.c` — `psram_qmi_apply_timing()` QMI CLKDIV rescale.
- `apps/doom/Makefile` — `-DFEATURE_SOUND`.
- `apps/doom/i_picos_sound.c` — Phase-0 producer-side [DOOM-AUDIO] log.
- `apps/doom/dg_picos.c` — first-3-frames render/flush breadcrumbs.
- `apps/doom/src` (submodule) — i_video.c (rgb565 palette), i_system.c
  (MIN_RAM 2 MiB), i_sound.c (SDL_mixer guard). Needs a submodule commit +
  pointer bump.
- `apps/doom/main.elf` — rebuilt with sound enabled.

## Remaining tasks

| # | Status | Phase | Subject |
| -- | ------ | ----- | ------- |
| 1 | DONE | 0 | Add Doom audio instrumentation (firmware + Doom side) |
| — | DONE | — | Root-cause launch crash (QMI PSRAM overclock) + fix |
| — | DONE | — | Root-cause silent audio (FEATURE_SOUND) + fix |
| — | DONE | — | Validate Doom audio on hardware (SFX full-rate; music can't keep up) |
| — | TODO | — | **QMI quad-mode PSRAM** — M1 is in reset serial mode (1-bit, ~6 MB/s at 50 MHz); QPI 0xEB fast-read would give ~4× bandwidth, likely making OPL music viable and speeding every PSRAM app. Needs QPI-entry sequencing + RXDELAY tuning + validation |
| — | TODO | — | Re-enable Doom music (drop -nomusic) once QMI quad mode or a lighter OPL core lands |
| — | TODO | — | Watch Core-1 stack: OPL emulator + mixer run on the 4 KB Core-1 MSP |
| 2 | TODO | 1.A | Re-evaluate Core 1 timer `-1` → `-5` ms (src/main.c) after audio validation |
| 3 | TODO | 1.B | Make `sys_setAudioCallback` use `atomic_store` |
| 4 | TODO | 2.A | Add `tcp_close_all()` mirroring `http_close_all()` |
| 5 | TODO | 2.B | Call `http_close_all()` on app exit (not only re-entry) |
| 6 | TODO | 2.C | Fix REPL cross-app leak (`lua_bridge_repl_deinit`) |
| 7 | TODO | 2.D | Track / close FATFS handles on Lua-app exit |
| 8 | TODO | 3.A | Mark `dma_audio_irq_handler` `__time_critical_func` |
| 9 | TODO | 3.B | Batch mono MP3 ring writes |
| 10 | TODO | 3.C | Add `-ffunction-sections -fdata-sections` to `libmad_ram` |
| 11 | TODO | — | Build verify firmware + simulator + pytest harness |
| — | TODO | — | Decide fate of CODEWATCH + Phase-0 logs once stable (remove/gate) |

## Verification matrix

- Doom launch: survives ≥4 min at 300 MHz with `[LAUNCHER] QMI PSRAM
  clkdiv=6` in the log and zero CODEWATCH CORRUPT events.
- Doom audio: [DOOM-AUDIO] and [AUDIO] lines every ~2 s; zero underruns
  over 30 s; SFX + title music audible.
- Other 300 MHz apps (gbc, tic-80, c64, dos86): spot-check they still run.
- Cross-heap safety: launch three apps in sequence (Lua audio demo →
  Doom → Lua HTTP demo) and confirm PSRAM + SRAM free returns to baseline.
- Simulator still builds (verified) and pytest harness from `dc2e7751`
  still passes.
