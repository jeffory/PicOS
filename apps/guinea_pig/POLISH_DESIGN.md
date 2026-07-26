# Guinea Pig Run — Presentation Polish Pass

Date: 2026-07-26
Status: Approved by user (2026-07-26)
Scope: `apps/guinea_pig/` only (Lua app, sprites, new sfx). No firmware changes.

## Context

Guinea Pig Run is a Lua platformer (~1,600 lines, `apps/guinea_pig/main.lua`) with solid
architecture (scene system, camera, particles, enemies, abilities). Its presentation
needs a polish pass. All new raster assets are generated with the PixelLab MCP; all
audio is synthesized procedurally with a committed Python script. Existing good art
is kept (guinea pig sheets, house, clouds, hawk, hay pile, veggies, most props).

### Confirmed defects / weaknesses

1. **Ground tile gaps (root cause known).** `sprites/tile_grass_top.png` and
   `sprites/tile_earth.png` have magenta (transparent-keyed) 1–2 px borders; only
   ~13 of each 16 px tile is opaque, so `drawTiled` leaves vertical gap stripes.
2. **Off-theme / low-quality parallax art.** `mountain_far.png` is a flat blue blob;
   `island_distant.png` is a tropical island (wrong theme for a backyard guinea pig
   game); hills are crude procedural solid triangles.
3. **Bland title screen.** Three flat color bands + text + 3× nearest-neighbor sprite.
4. **Sprinkler ("hose") never changes direction.** `dir` is fixed at spawn
   (`1` or `-1`). Water is procedural blue rectangles.
5. **Sounds are raw `playTone()` square beeps.** Firmware supports WAV samples
   (`pc.sound.sampleplayer`, ≤4 simultaneous, ≤64 KB each, rate/volume control)
   and a streaming `pc.sound.fileplayer` for BGM.

## Decisions (from brainstorming)

- **Sound source: procedural chiptune synthesis.** A Python script
  (`apps/guinea_pig/tools/gen_sfx.py`) synthesizes WAVs (square/triangle/noise
  oscillators, ADSR envelopes, pitch sweeps, arpeggios). Offline, deterministic,
  no API keys, no licensing risk. Includes a looping BGM track.
  Alternatives rejected: Kenney CC0 download (less chiptune-pure), LLM audio
  services (need API keys, overkill).
- **Art theme: backyard garden.** Picket fence, hedges, flower/veggie beds,
  distant trees — matches veggies, sprinkler, broccoli enemies, and the pig
  reaching its house. Title screen = golden-hour garden scene.
  Alternatives rejected: open meadow (less specific), keep mountains (still
  incoherent with the rest of the game).

## Assets to generate (PixelLab MCP)

### Parallax layers (garden, back → front)
| File | Size | Scroll factor | Content |
|---|---|---|---|
| `sprites/bg_trees_far.png` | 320×64 | 0.10× | Distant tree/hedge silhouettes, low contrast |
| `sprites/bg_fence_mid.png` | 320×56 | 0.25× | Picket fence with bushes |
| `sprites/bg_garden_near.png` | 320×40 | 0.50× | Flower/veggie garden rows at the ground line |

- Delete/replace: `mountain_far.png`, `mountain_mid.png`, `island_distant.png`.
- Keep clouds (`cloud_large/small`) at 0.35×/0.55×.
- Sky stays procedural (gradient bands) retuned to warm garden-morning colors.
- All parallax strips must be horizontally tileable; verified pixel-by-pixel
  (left edge column == right edge column continuity) after generation.

### Title screen
| File | Size | Content |
|---|---|---|
| `sprites/title_bg.png` | 320×320 | Full garden scene at golden hour: fence, grass, the igloo house, soft light. Fully opaque (no transparency). |
| `sprites/title_logo.png` | ~200×48 | "GUINEA PIG RUN" chunky pixel logo w/ paw or veggie accent. Fallback: `create_font` + in-code lettering. |

### Ground tiles
- `create_sidescroller_tileset`: grass-topped brown earth platform set,
  **seamless by construction**, including left/right end-cap tiles.
- Post-process: verify every tile is fully opaque (zero magenta pixels) and
  L/R and T/B edges match for seamless wrap. Fail the build if not.

### Sprinkler
| File | Size | Content |
|---|---|---|
| `sprites/sprinkler_body.png` | 32×32 | Garden sprinkler on a spike (replaces hand-held spray-gun `hose_nozzle.png`) |
| `sprites/water_arc.png` | 128×24 strip (4× 32×24 frames) | Arcing droplet stream animation |
| `sprites/water_drop.png` | 8×8 | Single droplet particle |

### HUD / dressing
| File | Size | Content |
|---|---|---|
| `sprites/heart.png` | 12×12 | Heart icon for HP (replaces procedural block hearts) |
| `sprites/veggie_icon.png` | 12×12 | Small carrot icon for the veggie counter |

### Audio (`sfx/*.wav`, 22050 Hz 16-bit mono PCM, each < 64 KB)
jump, popcorn_jump, collect_carrot, collect_capsicum, collect_zucchini
(3 pitched chime variants), collect_powerup, dash, squeak (charged "WHEEK"),
damage, hawk_screech, water_burst, enemy_defeat, win_jingle, game_over,
menu_move, menu_select, plus `bgm.wav` (~8 s chiptune loop, streamed).

## Code changes (`main.lua` and new `tools/`)

### Sprinkler behavior (was "hose")
- `dir` becomes dynamic: sweeps −1 ↔ +1 on a timer (ease at extremes, brief
  pause at each end of the sweep, e.g. 1.2 s travel + 0.6 s dwell).
- Water drawn from `water_arc.png` frame strip, flipX/position follows current
  direction; droplet particles (`water_drop.png`) spawn at the arc's end.
- Push force on player follows current spray direction (unchanged magnitude).
- `water_burst` SFX on activation; soft loop optional if a sample slot is free.
- Hitbox tracks the current arc (same coverage as today, swept over the cycle).

### Ground rendering
- `drawTiled` with the new seamless set; left/right end caps at platform ends.
- Fallback procedural path kept for robustness.

### Parallax
- Remove procedural hills + mountains/islands drawing.
- Draw the 3 new garden layers, each wrapped horizontally (drawn twice to cover
  the seam), factors 0.10/0.25/0.50. Clouds on top at 0.35/0.55.
- Retune sky gradient bands to warm morning colors.

### Title screen
- `title_bg.png` full-screen, `title_logo.png` centered upper third.
- Animated guinea pig run cycle crossing the bottom periodically.
- Blinking "Press ENTER to Start"; instructions in a tidy footer panel;
  high score shown; ESC to exit (unchanged behavior).

### Win / game-over screens
- Win: sky + garden parallax layers drawn statically (no camera) + house sprite
  + win jingle + stats panel (score, veggies, all-veggies bonus, high score).
- Game over: dimmed backdrop + panel + game_over SFX.

### HUD
- `heart.png` icons for HP (full/empty), `veggie_icon.png` next to count.
- Power-up timer bars kept, restyled to match.

### Sound engine
- Replace `snd_*` playTone helpers with an SFX table: preload one
  `pc.sound.sampleplayer` per effect at boot.
- Priority under the 4-slot hardware cap: damage > collect > jump > ambience.
  If all slots busy, lowest-priority in-flight effect is dropped.
- BGM via `pc.sound.fileplayer` with loop: ~70% volume on title/win, ~40% in
  gameplay. Stopped when leaving the app.
- Graceful fallback to `playTone` per effect if its WAV fails to load.

## Pipeline & verification

- Work happens in git worktree `.claude/worktrees/guinea-pig-polish`
  (branch `worktree-guinea-pig-polish`).
- `apps/guinea_pig/tools/gen_sfx.py` — audio synth (stdlib `wave`/`math` only).
- `apps/guinea_pig/tools/postprocess.py` — alpha→magenta flattening for PixelLab
  PNGs, opaque-border enforcement/verification for tiles, edge-continuity check
  for parallax strips. Committed; assets are reproducible.
- Test loop (PicOS MCP, simulator):
  1. `push_app` stages `apps/guinea_pig` to the sim SD (whole directory, incl.
     new `sfx/`).
  2. `launch_app` "Guinea Pig Run" → screenshot title.
  3. keypress-drive into the level; screenshot at several camera offsets to
     verify tile seams, sprinkler oscillation (two screenshots ~1 s apart must
     show different spray directions), parallax motion.
  4. Drive to win screen (or temp debug shortcut) and screenshot.
  5. Byte-diff screenshots (`save_path` + PIL compare) where motion is expected.
- No gameplay redesign: physics, level layout, enemies, and abilities are
  unchanged except the sprinkler sweep. Feel changes are presentation-only
  (particles, sounds).

## Out of scope

- Firmware changes, new Lua APIs, simulator changes.
- Character art rework (guinea pig sheets, enemies) — current art is good.
- New levels, new enemies, new abilities.
- MOD-tracker music (sampled chiptune WAV chosen instead).

## Risks / notes

- 4-simultaneous-sample hardware cap → priority drop logic above; BGM uses the
  fileplayer (separate path), so it doesn't consume a sample slot.
- PixelLab output is RGBA; flatten to magenta (255,0,255) key in postprocess —
  the game's `setTransparentColor(MAGENTA)` convention requires it.
- Lua heap is PSRAM-backed; total new WAV payload (~20 × ≤64 KB) fits, but
  samples are loaded once at boot and never per-frame.
- `main.lua` stays a single file (~+250 lines) — consistent with current app
  structure; no new module split introduced by this pass.
