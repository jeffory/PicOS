# Guinea Pig Run — Presentation Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix ground-tile gaps, rework parallax/title/sprinkler/HUD/win screens, and replace beep SFX with synthesized chiptune WAVs + BGM, per `apps/guinea_pig/POLISH_DESIGN.md`.

**Architecture:** Lua-only game changes in `apps/guinea_pig/main.lua`; new raster assets generated via PixelLab MCP then post-processed by committed Python tools; audio synthesized by a committed Python script into a ≤64 KB SFX bank (play-range indexed) + a streamed BGM WAV. Two small firmware sound fixes land as separate commits and are mirrored into `simulator/sim_audio.c`.

**Tech Stack:** PicOS Lua API (v5), PixelLab MCP (`create_map_object`, `create_sidescroller_tileset`, `animate_object`), Python 3 + Pillow + stdlib `wave`, PicOS simulator via `picos` MCP, arm-none-eabi firmware build.

## Global Constraints

- Worktree: `/home/keith/Projects/PicOS/.claude/worktrees/guinea-pig-polish`, branch `worktree-guinea-pig-polish`. All paths below are relative to it.
- `docs/` is the wiki submodule — never commit there. Spec = `apps/guinea_pig/POLISH_DESIGN.md`.
- Transparency key is magenta `disp.rgb(255,0,255)`; every sprite PNG must be flattened so transparent pixels become exactly `(255,0,255)` RGB (no alpha channel).
- All WAV output: **11025 Hz, 16-bit, mono** (uniform rate is mandatory — the firmware playback timer interval comes from the last-played sample's native rate).
- SFX bank data chunk must be ≤ 65536 bytes (`SOUND_MAX_SAMPLE_SIZE`); spill to `bank2.wav` if needed.
- No gameplay redesign: physics, level layout, enemy set, abilities unchanged except the sprinkler sweep.
- Sim workflow: stage app with `push_app` **before** starting the simulator (launcher caches the app list at boot). Screenshots are byte-diffed (save_path + PIL compare), never eyeballed for motion.
- Sim sound: `simulator/sim_audio.c` mirrors `src/drivers/sound.c` — firmware sound changes are ported to both files.
- Lua sound loads must use `pc.sound.sample(path)` (sample userdata, GC-frees correctly), never `pc.sound.sampleplayer(path)` (leaks the sample — fixed in firmware Task 3, but the game must run on current firmware).
- Commit after every task. Firmware commits are separate from app commits.

## Sim/MCP quick reference (from project memory, verified)

- Start sim: `mcp__picos__start_simulator` with explicit `sd_card_path` (e.g. `<worktree>/simulator/assets/sd_card`), `headless: true`.
- Stage app: `mcp__picos__push_app` with `local_dir: <worktree>/apps/guinea_pig` — ships the whole directory including `sfx/`.
- Keys: `keypress("enter")`, `keypress("right")`, `keypress("down 5x")`, menu key is `"menu"` (not "sym").
- If the sim behaves like hardware (no SimulatorWiFi in `get_status`), the device fell through to hardware — always confirm `get_status` shows the simulator before injecting keys.
- Fresh worktrees lack `build_sim` and submodule checkouts; Task 1 sets this up.

---

### Task 1: Build/asset tooling + worktree setup

**Files:**
- Create: `apps/guinea_pig/tools/postprocess.py`
- Create: `apps/guinea_pig/tools/check_assets.py`

**Interfaces:**
- Produces (used by all later asset tasks):
  - `postprocess.py flatten <in.png> <out.png>` — alpha→magenta flatten (also strips alpha channel; output RGB).
  - `postprocess.py opaque <file.png>` — exit 1 if any pixel equals (255,0,255); used for tiles (must have zero transparent pixels).
  - `postprocess.py slice <sheet.png> <cell_w> <cell_h> <col> <row> <out.png>` — extract one grid cell.
  - `check_assets.py` — validates the full `sprites/` + `sfx/` manifest; exits 1 on any failure.

- [ ] **Step 1: Rsync submodule/third_party content from the main checkout**

Fresh worktrees don't populate submodules or downloaded `third_party` content (recurring gotcha). The simulator and firmware builds need them:

```bash
cd /home/keith/Projects/PicOS/.claude/worktrees/guinea-pig-polish
for d in $(git config -f .gitmodules --get-regexp path | awk '{print $2}'); do
  mkdir -p "$d"
  rsync -a "/home/keith/Projects/PicOS/$d/" "$d/" 2>/dev/null || true
done
rsync -a /home/keith/Projects/PicOS/third_party/ third_party/ 2>/dev/null || true
ls third_party/ | head -5
```

Expected: `third_party` populated; if any submodule path is still empty after rsync, run `git submodule update --init` in the MAIN checkout (never in the worktree), then re-rsync.

- [ ] **Step 2: Build the simulator once**

```bash
cd /home/keith/Projects/PicOS/.claude/worktrees/guinea-pig-polish
make simulator -j4 2>&1 | tail -5
ls -la build_sim/picos_simulator
```

Expected: `build_sim/picos_simulator` exists. (If `make simulator` doesn't auto-create `build_sim`, use `cmake -B build_sim -S simulator && make -C build_sim -j4` per repo docs.)

- [ ] **Step 3: Write `tools/postprocess.py`**

```python
#!/usr/bin/env python3
"""Post-process PixelLab PNGs for PicOS: flatten alpha to the magenta
transparency key, verify tile opacity, slice grid sheets.

Usage:
  postprocess.py flatten <in.png> <out.png>
  postprocess.py opaque  <file.png>           # exit 1 if any magenta pixel
  postprocess.py slice   <sheet.png> <cw> <ch> <col> <row> <out.png>
"""
import sys
from PIL import Image

KEY = (255, 0, 255)

def flatten(src, dst):
    img = Image.open(src)
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        out = Image.new("RGB", img.size, KEY)
        out.paste(img, (0, 0), img)          # alpha-composite over magenta
    else:
        out = img.convert("RGB")
    out.save(dst)
    print(f"flatten: {dst} ({out.width}x{out.height})")

def opaque(path):
    img = Image.open(path).convert("RGB")
    n = sum(1 for p in img.getdata() if p == KEY)
    if n:
        print(f"opaque: FAIL {path}: {n} transparent pixels")
        sys.exit(1)
    print(f"opaque: OK {path}")

def slice_cell(sheet, cw, ch, col, row, out):
    img = Image.open(sheet)
    cell = img.crop((col * cw, row * ch, (col + 1) * cw, (row + 1) * ch))
    cell.save(out)
    print(f"slice: {out} ({cw}x{ch} from {col},{row})")

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "flatten":
        flatten(sys.argv[2], sys.argv[3])
    elif cmd == "opaque":
        opaque(sys.argv[2])
    elif cmd == "slice":
        slice_cell(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]),
                   int(sys.argv[5]), int(sys.argv[6]), sys.argv[7])
    else:
        sys.exit(f"unknown command {cmd}")
```

- [ ] **Step 4: Prove the opaque checker catches the current (buggy) tiles**

```bash
cd apps/guinea_pig
python3 tools/postprocess.py opaque sprites/tile_grass_top.png; echo "exit=$?"
python3 tools/postprocess.py opaque sprites/tile_earth.png; echo "exit=$?"
```

Expected: both print FAIL and `exit=1` (64 transparent pixels each — the ground-gap bug). This is the failing test that Task 2 makes pass.

- [ ] **Step 5: Write `tools/check_assets.py`**

```python
#!/usr/bin/env python3
"""Validate guinea_pig sprites/ and sfx/ against the shipping manifest."""
import os, struct, sys, wave

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.normpath(os.path.join(HERE, ".."))

SPRITES = {  # name -> (w, h) or None to skip size check
    "tile_grass_top.png": (16, 16), "tile_earth.png": (16, 16),
    "tile_edge_l.png": (16, 16), "tile_edge_r.png": (16, 16),
    "bg_trees_far_1.png": None, "bg_trees_far_2.png": None,
    "bg_fence_mid_1.png": None, "bg_fence_mid_2.png": None,
    "bg_garden_near_1.png": None, "bg_garden_near_2.png": None,
    "sprinkler_body.png": (32, 32), "water_arc.png": (256, 24),
    "title_logo.png": None, "heart.png": (32, 32),
}
OPAQUE = {"tile_grass_top.png", "tile_earth.png",
          "tile_edge_l.png", "tile_edge_r.png"}

def check_wav(path, max_data=65536):
    with wave.open(path, "rb") as w:
        ok = (w.getframerate() == 11025 and w.getsampwidth() == 2
              and w.getnchannels() == 1)
        frames = w.getnframes()
    size = os.path.getsize(path)
    if not ok:
        return f"{path}: must be 11025Hz 16-bit mono"
    if os.path.basename(path).startswith("bank") and frames * 2 > max_data:
        return f"{path}: bank data {frames*2} > {max_data}"
    return None

def main():
    from PIL import Image
    errors = []
    for name, dims in SPRITES.items():
        p = os.path.join(APP, "sprites", name)
        if not os.path.exists(p):
            errors.append(f"missing sprite {name}"); continue
        img = Image.open(p).convert("RGB")
        if dims and img.size != dims:
            errors.append(f"{name}: size {img.size} != {dims}")
        if name in OPAQUE and any(px == (255, 0, 255) for px in img.getdata()):
            errors.append(f"{name}: has transparent pixels (tile gap bug)")
    for name in ("bank1.wav", "bgm.wav"):
        p = os.path.join(APP, "sfx", name)
        if not os.path.exists(p):
            errors.append(f"missing sfx {name}"); continue
        err = check_wav(p)
        if err: errors.append(err)
    rp = os.path.join(APP, "sfx", "sfx_ranges.lua")
    if not os.path.exists(rp):
        errors.append("missing sfx/sfx_ranges.lua")
    if errors:
        print("\n".join("FAIL " + e for e in errors)); sys.exit(1)
    print("check_assets: all OK")

if __name__ == "__main__":
    main()
```

- [ ] **Step 6: Commit**

```bash
git add apps/guinea_pig/tools/
git commit -m "tools: asset post-processing + manifest checker for guinea pig polish"
```

---

### Task 2: Ground tileset — generate, slice, wire (fixes the gap bug)

**Files:**
- Create (generated): `apps/guinea_pig/sprites/tile_grass_top.png`, `tile_earth.png` (overwritten), `tile_edge_l.png`, `tile_edge_r.png`
- Modify: `apps/guinea_pig/main.lua` (sprite loads ~line 202-203; platform drawing ~lines 1192-1217)
- Raw download: `apps/guinea_pig/tools/raw/sidescroller_tileset.png`

**Interfaces:**
- Consumes: `postprocess.py` (Task 1).
- Produces: `sprites.tile_edge_l`, `sprites.tile_edge_r` in `main.lua`'s `sprites` table; ground draw uses 4 tiles. `check_assets.py` manifest already lists them.

- [ ] **Step 1: Generate the tileset with PixelLab**

```
mcp__pixellab__create_sidescroller_tileset(
  lower_description="rich brown earth dirt with small stones, pixel art",
  transition_description="lush short green grass, pixel art",
  transition_size=0.25,
  tile_size=16,
  outline="single color outline",
  shading="basic shading",
  detail="medium detail",
  seed=7)
```
Then `get_sidescroller_tileset(tileset_id)` until completed; download the tileset PNG to `apps/guinea_pig/tools/raw/sidescroller_tileset.png`.

- [ ] **Step 2: Inspect the grid and pick cells**

Read the downloaded PNG (Read tool renders it). Identify cells for: grass-top surface, plain earth, left platform edge (grass-topped), right platform edge (grass-topped). Record their (col,row).

- [ ] **Step 3: Slice, flatten, and verify opacity**

```bash
cd apps/guinea_pig
python3 tools/postprocess.py slice tools/raw/sidescroller_tileset.png 16 16 <c> <r> tools/raw/_grass.png
python3 tools/postprocess.py slice tools/raw/sidescroller_tileset.png 16 16 <c> <r> tools/raw/_earth.png
python3 tools/postprocess.py slice tools/raw/sidescroller_tileset.png 16 16 <c> <r> tools/raw/_el.png
python3 tools/postprocess.py slice tools/raw/sidescroller_tileset.png 16 16 <c> <r> tools/raw/_er.png
for f in _grass _earth _el _er; do python3 tools/postprocess.py flatten tools/raw/$f.png tools/raw/$f.png; done
python3 tools/postprocess.py flatten tools/raw/_grass.png sprites/tile_grass_top.png
python3 tools/postprocess.py flatten tools/raw/_earth.png sprites/tile_earth.png
python3 tools/postprocess.py flatten tools/raw/_el.png   sprites/tile_edge_l.png
python3 tools/postprocess.py flatten tools/raw/_er.png   sprites/tile_edge_r.png
python3 tools/postprocess.py opaque sprites/tile_grass_top.png
python3 tools/postprocess.py opaque sprites/tile_earth.png
python3 tools/postprocess.py opaque sprites/tile_edge_l.png
python3 tools/postprocess.py opaque sprites/tile_edge_r.png
```

Expected: all four `opaque: OK` (Task 1's failing test now passes).

- [ ] **Step 4: Wire edge caps in `main.lua`**

After the existing tile loads (`load_sprite("tile_earth", "tile_earth.png")` ~line 203) add:

```lua
load_sprite("tile_edge_l", "tile_edge_l.png")     -- 16x16 left end cap
load_sprite("tile_edge_r", "tile_edge_r.png")     -- 16x16 right end cap
```

Replace the two tiled-ground drawing blocks (the `plat.ground` branch ~line 1193 and the floating-platform branch ~line 1207, both currently `tile_grass:drawTiled(sx, sy, plat.w, 16)` + earth below) with a shared helper placed before `draw_world`:

```lua
local function draw_tiled_platform(sx, sy, w, h)
    if not (sprites.tile_grass and sprites.tile_earth) then
        disp.fillRect(sx, sy + 3, w, h - 3, BROWN)
        disp.fillRect(sx, sy, w, 4, GRASS_GREEN)
        return
    end
    local body_x, body_w = sx, w
    if sprites.tile_edge_l and w >= 16 then
        sprites.tile_edge_l:draw(sx, sy)
        body_x = body_x + 16
        body_w = body_w - 16
    end
    if sprites.tile_edge_r and w >= 32 then
        sprites.tile_edge_r:draw(sx + w - 16, sy)
        body_w = body_w - 16
    end
    if body_w > 0 then
        sprites.tile_grass:drawTiled(body_x, sy, body_w, 16)
        if h > 16 then
            sprites.tile_earth:drawTiled(body_x, sy + 16, body_w, h - 16)
            -- fill strip under the caps too
            sprites.tile_earth:drawTiled(sx, sy + 16, 16, h - 16)
            sprites.tile_earth:drawTiled(sx + w - 16, sy + 16, 16, h - 16)
        end
    end
end
```

Call it from both branches: `draw_tiled_platform(sx, sy, plat.w, plat.h)` (keeps the procedural fallback inside the helper; delete the old fallback blocks).

- [ ] **Step 5: Sim verify — no gaps at multiple camera offsets**

Stage and run:
```
mcp__picos__push_app(local_dir=<worktree>/apps/guinea_pig)
mcp__picos__start_simulator(sd_card_path=<worktree>/simulator/assets/sd_card, headless=true)
mcp__picos__launch_app("Guinea Pig Run")
mcp__picos__keypress("enter")            # menu -> play
mcp__picos__screenshot(save_path=/tmp/gp_t0.png)
mcp__picos__keypress("right", count=30)  # hold right ~3s (100ms per press)
mcp__picos__screenshot(save_path=/tmp/gp_t1.png)
```
Then pixel-scan the ground band (y=280..295 in world → screen rows vary; scan full frame) for vertical sky-colored stripes cutting through the ground:

```bash
python3 - <<'EOF'
from PIL import Image
import sys
for f in ("/tmp/gp_t0.png", "/tmp/gp_t1.png"):
    img = Image.open(f).convert("RGB")
    w, h = img.size
    px = img.load()
    # ground band on screen: find rows that are mostly brown/green
    bad = 0
    for x in range(w):
        col = [px[x, y] for y in range(int(h*0.85), h)]
        skyish = sum(1 for r, g, b in col if b > 150 and b > r + 40)
        if skyish > len(col) * 0.6:
            bad += 1
    print(f, "sky-stripe columns in ground band:", bad)
    sys.exit(1 if bad > 0 else 0)
EOF
```

Expected: `sky-stripe columns in ground band: 0` for both.

- [ ] **Step 6: Commit**

```bash
git add apps/guinea_pig/sprites/tile_*.png apps/guinea_pig/main.lua apps/guinea_pig/tools/raw/
git commit -m "guinea_pig: seamless sidescroller ground tiles + edge caps (fixes gap bug)"
```

---

### Task 3: Firmware sound fixes (leak + hardening + slot headroom)

**Files:**
- Modify: `src/drivers/sound.h` (struct + define)
- Modify: `src/drivers/sound.c` (`sound_init`, `sound_player_destroy`)
- Modify: `src/os/lua_bridge_sound.c` (`l_sound_sampleplayer_new`, path branch)
- Modify: `simulator/sim_audio.c` (mirror the two `sound.c` changes)

**Interfaces:**
- Consumes: nothing (independent of Tasks 1-2).
- Produces: `sound_player_t.owns_sample` (bool, internal to firmware + sim); `SOUND_MAX_SAMPLES = 8`. No PicoCalcAPI change (opaque pointers) — no native-app ABI break.

- [ ] **Step 1: `sound.h` changes**

```c
#define SOUND_MAX_SAMPLES 8
```
and in `sound_player_t`, after `sound_sample_t *sample;`:
```c
    bool owns_sample;   // path-constructed players free their sample on destroy
```

- [ ] **Step 2: `sound.c` — free owned sample on destroy; harden `sound_init`**

```c
void sound_player_destroy(sound_player_t *player) {
    if (player) {
        sound_player_stop(player);
        if (player->owns_sample && player->sample) {
            sound_sample_destroy(player->sample);
        }
        player->sample = NULL;
        player->owns_sample = false;
    }
}
```

In `sound_init()`, before `memset(&s_context, 0, sizeof(s_context));`:
```c
    // Reclaim any loaded sample data before dropping the pointers (app exit
    // must not leak PSRAM, even for path-constructed samples).
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        sound_sample_t *s = s_context.samples[i];
        if (s) {
            if (s->data) umm_free(s->data);
            free(s);
            s_context.samples[i] = NULL;
        }
    }
```
(`sound_sample_t` is allocated with `calloc` in `sound_sample_create`, its `data` with `umm_malloc` — match the existing free discipline in `sound_sample_destroy`.)

- [ ] **Step 3: `lua_bridge_sound.c` — mark ownership in the path constructor**

In `l_sound_sampleplayer_new`, in the `lua_isstring` branch after `playerSetSample`:
```c
    if (sample)
        g_api.soundplayer->playerSetSample(player, sample);
    if (sample)
        ((sound_player_t *)player)->owns_sample = true;   // GC reclaims it
```
(The bridge already includes `../drivers/sound.h` for `sound_player_t`.)

- [ ] **Step 4: Mirror into `simulator/sim_audio.c`**

Apply the same `sound_player_destroy` body and `sound_init` hardening to the sim's implementations (it uses the shared `sound.h`, so the struct/define changes are automatic; only function bodies need porting).

- [ ] **Step 5: Build firmware + simulator**

```bash
cd /home/keith/Projects/PicOS/.claude/worktrees/guinea-pig-polish
make -C build -j4 2>&1 | tail -3        # firmware (build/ configured by user env)
make simulator -j4 2>&1 | tail -3
```
Expected: both link cleanly. If `build/` isn't configured in the worktree: `cmake -B build -DPICO_BOARD=pimoroni_pico_plus2_w_rp2350` first (needs `PICO_SDK_PATH`).

- [ ] **Step 6: Sim regression — leak path now works past 4 loads**

Push a throwaway probe app (or reuse `systest`): a Lua file that loads 6 samples via the LEAK path (`pc.sound.sampleplayer(APP_DIR .. "/sfx/bank1.wav")` — requires Task 4's bank; if not ready, generate a 0.1s WAV with postprocess-free Python) in a loop, calling `collectgarbage()` between iterations, then `sys.log("loads ok: 6")`. Run in sim.

Expected: all 6 loads succeed and the log prints (pre-fix, loads 5-6 returned `nil, "failed to load sample"`). Delete the probe after.

- [ ] **Step 7: Commit**

```bash
git add src/drivers/sound.h src/drivers/sound.c src/os/lua_bridge_sound.c simulator/sim_audio.c
git commit -m "fix(sound): free path-constructed samples on player GC; harden sound_init vs PSRAM leaks; raise SOUND_MAX_SAMPLES to 8"
```

---

### Task 4: `tools/gen_sfx.py` — chiptune synth engine, SFX bank, BGM

**Files:**
- Create: `apps/guinea_pig/tools/gen_sfx.py`
- Create (generated): `apps/guinea_pig/sfx/bank1.wav` (+ `bank2.wav` only on overflow), `apps/guinea_pig/sfx/bgm.wav`, `apps/guinea_pig/sfx/sfx_ranges.lua`

**Interfaces:**
- Produces:
  - `sfx_ranges.lua` returns a table: `{ jump = {bank=1, first=0, last=1322}, ... }` (frame offsets, inclusive; `bank` is 1 or 2).
  - `bankN.wav`: 11025 Hz 16-bit mono, data ≤ 65536 bytes each.
  - `bgm.wav`: 11025 Hz 16-bit mono, ~8 s seamless loop (streamed, not size-capped).
- Consumed by: Task 5 (Lua SFX engine reads `sfx_ranges.lua`), Task 6 (BGM).

- [ ] **Step 1: Write the synth engine + recipes**

Complete script (deterministic, stdlib-only):

```python
#!/usr/bin/env python3
"""Synthesize chiptune SFX + BGM for Guinea Pig Run.
Output: sfx/bank1.wav (+bank2.wav on overflow), sfx/bgm.wav, sfx/sfx_ranges.lua
Deterministic: fixed seed, stdlib wave/math/random only."""
import math, os, random, wave

RATE = 11025
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "sfx"))
os.makedirs(OUT, exist_ok=True)
rng = random.Random(42)

def osc(kind, f, t):
    ph = (f * t) % 1.0
    if kind == "square": return 1.0 if ph < 0.5 else -1.0
    if kind == "tri":    return 4 * abs(ph - 0.5) - 1.0
    if kind == "sine":   return math.sin(2 * math.pi * ph)
    if kind == "noise":  return rng.uniform(-1, 1)
    raise ValueError(kind)

def layer(kind, f0, f1, dur, vol=0.5, attack=0.005, decay=None, delay=0.0,
          vibrato_hz=0.0, vibrato_depth=0.0):
    """Render one layer; returns (start_frame, [samples])."""
    decay = dur if decay is None else decay
    n = int(dur * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        k = i / max(1, n - 1)
        f = f0 + (f1 - f0) * k
        if vibrato_hz:
            f *= 1 + vibrato_depth * math.sin(2 * math.pi * vibrato_hz * t)
        env = min(1.0, t / attack) if t < attack else max(0.0, 1 - (t - attack) / max(1e-6, decay - attack))
        out.append(vol * env * osc(kind, f, t))
    return int(delay * RATE), out

def mix(layers, total_dur):
    n = int(total_dur * RATE)
    buf = [0.0] * n
    for start, samples in layers:
        for i, s in enumerate(samples):
            if start + i < n:
                buf[start + i] += s
    peak = max(1e-6, max(abs(s) for s in buf))
    gain = 0.89 / peak if peak > 0.89 else 1.0
    return [int(max(-1, min(1, s * gain)) * 32767) for s in buf]

# name -> (total_dur, [layer(...)], gap_after_ms)
EFFECTS = [
    ("jump",         0.14, [layer("square", 300, 700, 0.12, 0.45)], 30),
    ("popcorn",      0.16, [layer("square", 660, 660, 0.06, 0.4),
                            layer("square", 880, 880, 0.06, 0.4, delay=0.07)], 30),
    ("collect_carrot",   0.12, [layer("square", 880, 1320, 0.10, 0.4)], 30),
    ("collect_capsicum", 0.12, [layer("square", 660, 990, 0.10, 0.4)], 30),
    ("collect_zucchini", 0.12, [layer("square", 550, 780, 0.10, 0.4)], 30),
    ("collect_powerup", 0.24, [layer("tri", f, f, 0.055, 0.5, delay=d)
        for d, f in [(0, 523), (0.06, 659), (0.12, 784), (0.18, 1047)]], 40),
    ("dash",         0.12, [layer("noise", 0, 0, 0.09, 0.35),
                            layer("square", 330, 180, 0.10, 0.3)], 30),
    ("squeak",       0.34, [layer("square", 1400, 500, 0.32, 0.45,
                                  vibrato_hz=25, vibrato_depth=0.06)], 40),
    ("damage",       0.26, [layer("square", 220, 110, 0.24, 0.5),
                            layer("noise", 0, 0, 0.10, 0.25)], 40),
    ("hawk_screech", 0.20, [layer("square", 1000, 1600, 0.18, 0.3,
                                  vibrato_hz=40, vibrato_depth=0.08),
                            layer("noise", 0, 0, 0.18, 0.15)], 30),
    ("water_burst",  0.30, [layer("noise", 0, 0, 0.28, 0.4, attack=0.03)], 40),
    ("enemy_defeat", 0.16, [layer("square", 500, 120, 0.14, 0.45),
                            layer("noise", 0, 0, 0.05, 0.3)], 30),
    ("menu_move",    0.05, [layer("square", 800, 800, 0.03, 0.3)], 20),
    ("menu_select",  0.10, [layer("square", 1000, 1400, 0.08, 0.35)], 20),
    ("win_jingle",   0.72, [layer("square", f, f, 0.13, 0.4, delay=d)
        for d, f in [(0, 784), (0.15, 1047), (0.30, 1319), (0.45, 1568)]]
        + [layer("tri", f / 2, f / 2, 0.13, 0.3, delay=d)
        for d, f in [(0, 784), (0.15, 1047), (0.30, 1319), (0.45, 1568)]], 60),
    ("game_over",    0.62, [layer("square", f, f, 0.17, 0.4, delay=d)
        for d, f in [(0, 330), (0.20, 262), (0.40, 220)]], 40),
]
```

Bank packer + BGM (same file, append):

```python
def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(int(s).to_bytes(2, "little", signed=True)
                               for s in samples))

def main():
    banks = {1: [], 2: []}
    ranges = {}
    for name, dur, layers, gap_ms in EFFECTS:
        smp = mix(layers, dur)
        gap = [0] * int(gap_ms * RATE / 1000)
        bank = 1 if 2 * (len(banks[1]) + len(smp) + len(gap)) <= 65536 else 2
        first = len(banks[bank])
        banks[bank] += smp + gap
        ranges[name] = (bank, first, first + len(smp) - 1)
    for b, data in banks.items():
        if data:
            write_wav(os.path.join(OUT, f"bank{b}.wav"), data)
            print(f"bank{b}.wav: {len(data)*2} bytes data")
    with open(os.path.join(OUT, "sfx_ranges.lua"), "w") as f:
        f.write("-- generated by tools/gen_sfx.py -- do not edit\nreturn {\n")
        for name, (bank, first, last) in ranges.items():
            f.write(f'  {name} = {{bank={bank}, first={first}, last={last}}},\n')
        f.write("}\n")

    # BGM: 4-bar loop, I-V-vi-IV in C, square arp + tri bass + noise hats
    BPM, SPB, BARS = 132, 60 / 132, 4
    CHORDS = [(261.6, 329.6, 392.0), (196.0, 246.9, 293.7),
              (220.0, 261.6, 329.6), (174.6, 220.0, 261.6)]
    bar = int(SPB * 4 * RATE)
    bgm = [0.0] * (bar * BARS)
    def add(samples, at): 
        for i, s in enumerate(samples):
            if at + i < len(bgm): bgm[at + i] += s
    for b_i, chord in enumerate(CHORDS):
        base = b_i * bar
        for note_i in range(8):                      # 8th-note arpeggio
            f = chord[note_i % 3] * 2
            st, s = layer("square", f, f, SPB / 2 * 0.9, 0.16)
            add(s, base + int(note_i * SPB / 2 * RATE))
        st, s = layer("tri", chord[0] / 2, chord[0] / 2, SPB * 3.6, 0.30)
        add(s, base)
        for hat in range(4):                         # quarter-note hats
            st, s = layer("noise", 0, 0, 0.03, 0.10)
            add(s, base + int(hat * SPB * RATE))
    peak = max(abs(s) for s in bgm)
    bgm16 = [int(s / peak * 0.7 * 32767) for s in bgm]
    write_wav(os.path.join(OUT, "bgm.wav"), bgm16)
    print(f"bgm.wav: {len(bgm16)*2} bytes ({len(bgm16)/RATE:.1f}s)")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it and validate**

```bash
cd apps/guinea_pig
python3 tools/gen_sfx.py
python3 tools/check_assets.py
```

Expected: `bank1.wav` ≤ 65536 bytes data (recipes above total ≈ 2.9 s ≈ 64 KB; if it spills, `bank2.wav` appears and is also valid); `sfx_ranges.lua` written; `check_assets: all OK` (sprite failures for not-yet-generated files are expected at this stage — WAV/ranges checks must pass; note the sprite FAILs and move on).

Also sanity-listen locally if a player is available (`aplay sfx/bank1.wav`) — optional.

- [ ] **Step 3: Commit**

```bash
git add apps/guinea_pig/tools/gen_sfx.py apps/guinea_pig/sfx/
git commit -m "guinea_pig: synthesized chiptune SFX bank + BGM + range table"
```

---

### Task 5: Lua SFX engine in `main.lua`

**Files:**
- Modify: `apps/guinea_pig/main.lua` (replace `snd_*` block ~lines 147-158; all call sites)

**Interfaces:**
- Consumes: `sfx/bankN.wav` + `sfx_ranges.lua` (Task 4).
- Produces: `sfx.play(name)` — plays effect by name with priority + playTone fallback; `sfx.update()` — reserved no-op (kept for symmetry, may be dropped). Sound call sites unchanged in shape: `snd_jump()` etc. become thin wrappers over `sfx.play("jump")`, so no other task's code needs to change.

- [ ] **Step 1: Replace the sound-helpers block**

Replace lines 147-158 (the `snd_*` playTone helpers) with:

```lua
-- Sound engine: SFX bank (play ranges) + playTone fallback
local sfx = { ok = false, banks = {}, players = {}, ranges = {}, busy = {} }
local SFX_PRIORITY = {  -- higher wins when stealing a busy player
    damage = 100, game_over = 95, win_jingle = 90, collect_powerup = 60,
    enemy_defeat = 55, collect_carrot = 50, collect_capsicum = 50,
    collect_zucchini = 50, popcorn = 40, jump = 30, dash = 30,
    squeak = 30, hawk_screech = 20, water_burst = 10,
    menu_move = 80, menu_select = 85,
}

local function sfx_init()
    local ok, ranges = pcall(dofile, APP_DIR .. "/sfx/sfx_ranges.lua")
    if not ok or type(ranges) ~= "table" then return end
    sfx.ranges = ranges
    for b = 1, 2 do
        local path = APP_DIR .. "/sfx/bank" .. b .. ".wav"
        if pc.fs.exists(path) then
            local s = pc.sound.sample(path)   -- sample userdata: GC-safe load
            if s then sfx.banks[b] = s end
        end
    end
    if not sfx.banks[1] then return end
    for i = 1, 4 do
        local p = pc.sound.sampleplayer(sfx.banks[1])
        if not p then break end
        sfx.players[i] = p
        sfx.busy[i] = 0
    end
    sfx.ok = #sfx.players > 0
end

local SFX_FALLBACK = {  -- name -> {freq, ms} for playTone when bank missing
    jump = {440, 80}, popcorn = {660, 60}, collect_carrot = {880, 100},
    collect_capsicum = {660, 100}, collect_zucchini = {550, 100},
    collect_powerup = {440, 80}, dash = {330, 60}, squeak = {1200, 200},
    damage = {220, 150}, hawk_screech = {1000, 120}, win_jingle = {440, 150},
    water_burst = {300, 120}, enemy_defeat = {480, 100},
    menu_move = {800, 30}, menu_select = {1000, 60}, game_over = {220, 250},
}

function sfx.play(name)
    local fb = SFX_FALLBACK[name]
    if not sfx.ok then
        if fb then pc.audio.playTone(fb[1], fb[2]) end
        return
    end
    local r = sfx.ranges[name]
    if not r or not sfx.banks[r.bank or 1] then
        if fb then pc.audio.playTone(fb[1], fb[2]) end
        return
    end
    local my_pri = SFX_PRIORITY[name] or 10
    local slot, lowest, lowest_i = nil, math.huge, 1
    for i, p in ipairs(sfx.players) do
        if not p:isPlaying() then slot = i break end
        if sfx.busy[i] < lowest then lowest = sfx.busy[i] lowest_i = i end
    end
    if not slot then
        if lowest >= my_pri then return end  -- keep higher-priority sound
        slot = lowest_i
    end
    local p = sfx.players[slot]
    p:stop()
    if sfx.banks[r.bank or 1] ~= p:getSample() then p:setSample(sfx.banks[r.bank or 1]) end
    p:setPlayRange(r.first, r.last)
    p:setVolume(100)
    p:play(1)
    sfx.busy[slot] = my_pri
end

sfx_init()
```

Then redefine the helpers as one-liners (same names → zero call-site churn):

```lua
local function snd_jump() sfx.play("jump") end
local function snd_popcorn() sfx.play("popcorn") end
local function snd_collect_carrot() sfx.play("collect_carrot") end
local function snd_collect_capsicum() sfx.play("collect_capsicum") end
local function snd_collect_zucchini() sfx.play("collect_zucchini") end
local function snd_collect_powerup() sfx.play("collect_powerup") end
local function snd_dash() sfx.play("dash") end
local function snd_squeak() sfx.play("squeak") end
local function snd_damage() sfx.play("damage") end
local function snd_hawk() sfx.play("hawk_screech") end
local function snd_win() sfx.play("win_jingle") end
```

Also: win scene's two timed `pc.audio.playTone(660/880, ...)` calls (win_scene update ~lines 1533-1534) — delete them (win_jingle already contains the arpeggio); add `sfx.play("game_over")` where `player.dead` is first set in `damage_player` (inside the `if player.hp <= 0` branch).

Note: `pc.fs.exists` — verify the exact Lua name at implementation time (`grep -n "exists" src/os/lua_bridge_fs.c`); if it's `pc.fs.fileExists`, use that.

- [ ] **Step 2: Sim verify**

Stage + launch; press enter (menu_select path via scene switch — if not wired, jump sound on BTN_UP in-game is enough): no Lua error screen; `get_log_buffer` shows no `sound:` errors. Fallback test: in the staged SD copy rename `sfx/bank1.wav` → `sfx/bank1.wav.bak`, relaunch app, press jump — app must not error (playTone path); then restore the name and re-push.

- [ ] **Step 3: Commit**

```bash
git add apps/guinea_pig/main.lua
git commit -m "guinea_pig: SFX bank engine with priorities + playTone fallback"
```

---

### Task 6: BGM wiring

**Files:**
- Modify: `apps/guinea_pig/main.lua` (scenes + main loop)

**Interfaces:**
- Consumes: `sfx/bgm.wav` (Task 4).
- Produces: `bgm.play(scene_name)` / `bgm.stop()` / `bgm.update()` (called from main loop). Scenes call `bgm.play("menu" | "play" | "win")` in their `enter`.

- [ ] **Step 1: Add the BGM module after the sfx block**

```lua
-- BGM: streamed loop via fileplayer; replay-on-finish (panels.lua pattern)
local bgm = { fp = nil, want = nil, restart = false }
local BGM_VOLUME = { menu = 70, play = 40, win = 70 }

function bgm.play(scene)
    bgm.want = scene
    if not bgm.fp then
        local ok, fp = pcall(pc.sound.fileplayer)
        if not ok or not fp then return end
        local loaded = pcall(function() fp:load(APP_DIR .. "/sfx/bgm.wav") end)
        if not loaded then return end
        bgm.fp = fp
        pcall(function()
            bgm.fp:setFinishCallback(function() bgm.restart = true end)
        end)
    end
    pcall(function() bgm.fp:setVolume(BGM_VOLUME[scene] or 50) end)
    if bgm.fp.isPlaying and not bgm.fp:isPlaying() then
        pcall(function() bgm.fp:play(1) end)
    end
end

function bgm.update()
    if bgm.fp and bgm.restart and bgm.want then
        bgm.restart = false
        pcall(function() bgm.fp:play(1) end)
    end
end

function bgm.stop()
    bgm.want = nil
    if bgm.fp then pcall(function() bgm.fp:stop() end) end
end
```

- [ ] **Step 2: Wire scenes + main loop**

- `menu_scene.enter`: add `bgm.play("menu")`
- `play_scene.enter`: add `bgm.play("play")`
- `win_scene.enter`: add `bgm.play("win")`
- main loop (after `game.scene.update(dt)`): add `bgm.update()`

Note the finish callback fires from the opcode hook — it only sets `bgm.restart`; `bgm.update()` performs the actual replay (never call audio APIs from the callback).

- [ ] **Step 3: Sim verify + commit**

Stage + launch; no Lua errors; menu→play→back transitions clean. (Sim audio is SDL — headless may be silent; this verifies logic only. Audible check happens in Task 12's hardware pass.)

```bash
git add apps/guinea_pig/main.lua
git commit -m "guinea_pig: streamed BGM loop with per-scene volume"
```

---

### Task 7: Garden parallax layers (replaces hills/mountains/islands)

**Files:**
- Create (generated): `apps/guinea_pig/sprites/bg_trees_far_1.png`, `bg_trees_far_2.png`, `bg_fence_mid_1.png`, `bg_fence_mid_2.png`, `bg_garden_near_1.png`, `bg_garden_near_2.png`
- Modify: `apps/guinea_pig/main.lua` (sprite loads ~line 204-206; parallax section ~lines 569-687; sky colors ~lines 53-55)

**Interfaces:**
- Consumes: `postprocess.py flatten`.
- Produces: `draw_garden_layers(ox)` replacing `draw_mountains(ox)` + `draw_hills(ox)`; play scene draw calls `draw_sky(); draw_garden_layers(ox); draw_parallax(ox)` (clouds unchanged).

- [ ] **Step 1: Generate the six cluster sprites**

Design refinement vs spec §Assets: layers are **discrete cluster sprites** placed at spaced world-x positions (same pattern as the old mountains) instead of continuously wrapped strips — avoids wrap-seam artifacts entirely.

```
mcp__pixellab__create_map_object(description="distant tree line silhouette cluster, leafy trees and bushes, desaturated blue-green, soft haze, pixel art, side view", view="side", width=160, height=72, no_background=true)  # ×2 variants (vary seed/description slightly)
mcp__pixellab__create_map_object(description="white wooden picket fence segment with green bushes behind, pixel art, side view", view="side", width=160, height=56, no_background=true)  # ×2
mcp__pixellab__create_map_object(description="vegetable garden bed row with carrot tops and flowers, pixel art, side view", view="side", width=128, height=32, no_background=true)  # ×2
```
Download each, `postprocess.py flatten` into the filenames above. (Map objects auto-delete after 8 h — download immediately with `get_map_object`.)

- [ ] **Step 2: Replace the parallax data + drawing code**

Delete `hills`, `mountains_far`, `mountains_mid`, `islands_distant` tables and `draw_hills`, `draw_mountains` functions (~lines 591-665). Add:

```lua
local bg_trees_far = {
    {x = 80, img = 1}, {x = 620, img = 2}, {x = 1180, img = 1},
    {x = 1760, img = 2}, {x = 2340, img = 1}, {x = 2920, img = 2},
}
local bg_fence_mid = {
    {x = 260, img = 1}, {x = 900, img = 2}, {x = 1540, img = 1},
    {x = 2180, img = 2}, {x = 2820, img = 1},
}
local bg_garden_near = {
    {x = 40, img = 1}, {x = 480, img = 2}, {x = 980, img = 1},
    {x = 1520, img = 2}, {x = 2060, img = 1}, {x = 2560, img = 2},
    {x = 2980, img = 1},
}

local function draw_cluster_set(set, imgs, base_y, ox, factor)
    for _, c in ipairs(set) do
        local img = imgs[c.img]
        if img then
            local _, ih = img:getSize()
            local sx = parallax_x(c.x, ox, factor)
            if sx > -170 and sx < SCREEN_W + 10 then
                img:draw(sx, base_y - ih)
            end
        end
    end
end

local function draw_garden_layers(ox)
    draw_cluster_set(bg_trees_far,  {sprites.bg_trees_far_1, sprites.bg_trees_far_2},  GROUND_Y + 8, ox, 0.10)
    draw_cluster_set(bg_fence_mid,  {sprites.bg_fence_mid_1, sprites.bg_fence_mid_2},  GROUND_Y + 4, ox, 0.25)
    draw_cluster_set(bg_garden_near,{sprites.bg_garden_near_1, sprites.bg_garden_near_2}, GROUND_Y + 2, ox, 0.50)
end
```

(`img:getSize()` returns w, h — used to bottom-align each cluster to the ground line. If the method name differs, check `l_graphics_image_getSize` in `src/os/lua_bridge_graphics.c`.)

Sprite loads: replace the mountain/island loads (~lines 204-206) with the six new `load_sprite` calls (`bg_trees_far_1/2`, `bg_fence_mid_1/2`, `bg_garden_near_1/2`).

- [ ] **Step 3: Retune the sky**

Replace SKY_LIGHT/MID/DARK definitions (~lines 53-55) with warmer morning tones:

```lua
local SKY_LIGHT = disp.rgb(255, 224, 160)   -- golden horizon glow
local SKY_MID = disp.rgb(150, 200, 235)
local SKY_DARK = disp.rgb(90, 160, 215)
```
And swap `draw_sky` band order so LIGHT is at the horizon (bottom), not top:

```lua
local function draw_sky()
    disp.fillRect(0, 0, SCREEN_W, 120, SKY_DARK)
    disp.fillRect(0, 120, SCREEN_W, 120, SKY_MID)
    disp.fillRect(0, 240, SCREEN_W, 80, SKY_LIGHT)
end
```
Update menu/win scene text background colors that referenced the old bands (`SKY_LIGHT` at top → use `SKY_DARK`-compatible bg args; the menu scene is rewritten in Task 9 — only fix compile-visible mismatches here).

- [ ] **Step 4: Sim verify layer motion ratios**

Screenshots at three camera offsets (fresh launch, after 40 right-presses, after 80): layers must move, far < mid < near in displacement. Byte-diff with PIL:

```bash
python3 - <<'EOF'
from PIL import Image, ImageChops
a = Image.open("/tmp/gp_p0.png").convert("RGB")
b = Image.open("/tmp/gp_p1.png").convert("RGB")
diff = ImageChops.difference(a, b)
bbox = diff.getbbox()
print("changed region:", bbox)
assert bbox, "parallax did not move"
EOF
```
Plus a screenshot read-through: no blue blob, no tropical island, fence/trees/garden visible behind gameplay.

- [ ] **Step 5: Commit**

```bash
git add apps/guinea_pig/sprites/bg_*.png apps/guinea_pig/main.lua
git commit -m "guinea_pig: garden parallax cluster layers + warm sky (drops blob mountains, tropical islands, triangle hills)"
```

---

### Task 8: Sprinkler rework (sweep + water art)

**Files:**
- Create (generated): `apps/guinea_pig/sprites/sprinkler_body.png`, `apps/guinea_pig/sprites/water_arc.png`
- Modify: `apps/guinea_pig/main.lua` (hose enemy update ~lines 966-977; hose draw ~lines 1268-1273; `draw_water_spray` ~lines 377-383; `draw_hose_nozzle` ~lines 367-374)

**Interfaces:**
- Consumes: `animate_object` frames (PixelLab), `postprocess.py flatten`.
- Produces: hose enemy gains `e.sweep` (phase timer) and dynamic `e.dir`; water drawn from `water_arc.png` strip. No interface changes to other tasks.

- [ ] **Step 1: Generate sprinkler art**

```
mcp__pixellab__create_map_object(description="garden lawn sprinkler on a metal spike, brass sprinkler head, pixel art, side view", view="side", width=32, height=32, no_background=true)
mcp__pixellab__create_map_object(description="arcing stream of water droplets spraying right, bright blue water with white sparkle, pixel art, side view", view="side", width=64, height=24, no_background=true)
```
Flatten the body to `sprites/sprinkler_body.png`. For the water: `animate_object(object_id=<water obj>, animation_description="water flowing and droplets pulsing along the arc", frame_count=4, mode="v3")`, wait, `get_object` → download the 4 frames (each 64×24) → assemble horizontally into `sprites/water_arc.png` (256×24 strip) with a tiny PIL script (reuse `postprocess.py slice`-style code inline), then flatten. If the animation result is poor after one retry, fall back to the single static frame duplicated ×4 and rely on procedural droplet animation (below).

- [ ] **Step 2: Rework hose behavior in `update_enemies`**

Replace the hose branch (~lines 966-977) with sweep logic:

```lua
        elseif e.type == "hose" then
            e.timer = e.timer + dt
            if e.active then
                if e.timer > 4.8 then e.active = false; e.timer = 0 end
                -- sweep: phase 0..1 across the active window, dwell at extremes
                local phase = clamp(e.timer / 4.8, 0, 1)
                local swing = math.sin(phase * math.pi * 2 - math.pi / 2) * 0.5 + 0.5
                e.sweep = swing                       -- 0=left .. 1=right
                e.dir = swing >= 0.5 and 1 or -1
                local spray_x = e.x - e.spray_w + swing * (e.spray_w - 8) + 8
                if not player.dead and not player.hiding and
                   aabb_overlap(player.x, player.y, player.w, player.h, spray_x, e.y - 4, 24, 16) then
                    player.vx = player.vx + e.dir * 2000 * dt
                end
                if not e.burst_done then sfx.play("water_burst"); e.burst_done = true end
            else
                e.burst_done = false
                if e.timer > 3.0 then e.active = true; e.timer = 0 end
            end
```

(`e.sweep` centers the 24px-wide jet hitbox within the old 48px envelope as it travels left↔right; coverage per instant shrinks but sweeps — the intended new feel. Update the two hose spawns to add `sweep = 0.5`; remove the spawn `dir` values.)

- [ ] **Step 3: Rework the drawing**

Replace `draw_hose_nozzle` and `draw_water_spray` with:

```lua
local function draw_sprinkler(sx, sy, sweep, active, t)
    if sprites.sprinkler_body then
        sprites.sprinkler_body:draw(sx - 8, sy - 16)
    else
        disp.fillRect(sx + 4, sy + 4, 8, 8, GREEN)
    end
    if not active then return end
    if sprites.water_arc then
        -- 256x24 strip, 4 frames of 64x24, arc always sprays right; flip for left
        local frame = math.floor(t * 12) % 4
        local ax = math.floor(sx - 56 + sweep * 48)   -- arc travels with sweep
        sprites.water_arc:draw(ax, sy - 8, {flipX = sweep < 0.5},
            {x = frame * 64, y = 0, w = 64, h = 24})
        -- droplets at the arc's landing point, following the sweep
        local dx = sweep >= 0.5 and (ax + 56) or (ax + 4)
        for i = 0, 2 do
            local dy = math.floor(math.sin(t * 10 + i * 2) * 3)
            disp.fillRect(dx + i * 3 - 3, sy - 2 + dy + i * 2, 2, 2, WATER_LIGHT)
        end
    else
        local jet_w = 24
        local jx = math.floor(sx - jet_w + sweep * (jet_w - 8) + 8)
        for i = 0, jet_w - 4, 6 do
            disp.fillRect(jx + i, sy + 1 + (i % 3), 4, 3, WATER_BLUE)
        end
    end
end
```
And the enemy draw branch (~line 1268): `draw_sprinkler(sx, sy, e.sweep or 0.5, e.active, game_time)`. Also delete the now-unused `load_sprite("hose_nozzle", "hose_nozzle.png")` line (~line 197) — Task 12 deletes the file.

- [ ] **Step 4: Sim verify direction change**

Launch into play, walk right to the first sprinkler (x=750 → ~40 right-presses from spawn with pauses). Two screenshots ~1.2 s apart:

```bash
python3 - <<'EOF'
from PIL import Image, ImageChops
a = Image.open("/tmp/gp_s0.png").convert("RGB")
b = Image.open("/tmp/gp_s1.png").convert("RGB")
d = ImageChops.difference(a, b)
print("changed bbox:", d.getbbox())
assert d.getbbox(), "sprinkler spray did not change"
EOF
```
Read one screenshot: spray must be on the opposite side / different position between frames; new sprinkler body visible instead of the green spray-gun.

- [ ] **Step 5: Commit**

```bash
git add apps/guinea_pig/sprites/sprinkler_body.png apps/guinea_pig/sprites/water_arc.png apps/guinea_pig/main.lua
git commit -m "guinea_pig: oscillating sprinkler with animated water arc"
```

---

### Task 9: Title screen

**Files:**
- Create (generated): `apps/guinea_pig/sprites/title_logo.png`
- Modify: `apps/guinea_pig/main.lua` (`menu_scene.draw` ~lines 1431-1457)

**Interfaces:**
- Consumes: garden layer sprites (Task 7), `sprites.house`, `sprites.gp_run_east`, logo.
- Produces: rewritten `menu_scene.draw`. No new interfaces.

- [ ] **Step 1: Generate the logo**

```
mcp__pixellab__create_map_object(description="GUINEA PIG RUN video game logo, chunky pixel art letters, warm orange-yellow letters with dark brown outline, small carrot accent, transparent background", view="side", width=256, height=64, no_background=true)
```
Flatten → `sprites/title_logo.png`. If the text comes back garbled (common with image models) after one retry, fall back: in-code title using the built-in font at 3× (`disp.drawText` scaled via a tiny NN blit helper already available as `drawScaledNN` on a rendered image is overkill — simpler fallback: draw "GUINEA PIG RUN" twice, offset, for a drop-shadow look) and skip the logo asset (remove from `check_assets.py` manifest).

- [ ] **Step 2: Rewrite `menu_scene.draw`**

Design refinement vs spec: no `title_bg.png` — the title is **composed from the game's own garden layers** (guaranteed coherent, no 320×320 generation risk):

```lua
    draw = function()
        draw_sky()
        draw_garden_layers(0)              -- static camera: full backdrop
        for _, c in ipairs(clouds_near) do
            draw_cloud(c.x % SCREEN_W, c.y * 0.6, c.w)
        end
        if sprites.title_logo then
            local lw = sprites.title_logo:getSize()
            sprites.title_logo:drawScaled(160 - lw / 2, 40, 0.75)
        else
            disp.drawText(66, 48, "GUINEA PIG RUN", DARK_BROWN, SKY_DARK)
            disp.drawText(64, 46, "GUINEA PIG RUN", YELLOW, SKY_DARK)
        end
        -- house + animated pig running home along the bottom
        draw_house(250, GROUND_Y - 36)
        local run_x = math.floor((game_time * 60) % (SCREEN_W + 80)) - 60
        local rf = math.floor(game_time * 10) % GP_RUN_FRAMES
        if sprites.gp_run_east then
            sprites.gp_run_east:draw(run_x, GROUND_Y - 16, nil,
                {x = rf * GP_FRAME_W, y = 0, w = GP_FRAME_W, h = GP_FRAME_H})
        end
        -- footer panel
        disp.fillRect(0, 236, SCREEN_W, 84, disp.rgb(30, 60, 30))
        disp.drawText(12, 244, "Arrows: Move   Up/Enter: Jump", WHITE, disp.rgb(30, 60, 30))
        disp.drawText(12, 258, "F1: Sonic Squeak (hold)", WHITE, disp.rgb(30, 60, 30))
        disp.drawText(12, 272, "F2: Dash   Down: Hide in hay", WHITE, disp.rgb(30, 60, 30))
        if high_score > 0 then
            disp.drawText(12, 288, "Best: " .. high_score, GOLD, disp.rgb(30, 60, 30))
        end
        if math.floor(game_time * 2) % 2 == 0 then
            disp.drawText(196, 288, "Press ENTER", YELLOW, disp.rgb(30, 60, 30))
        end
        disp.drawText(196, 302, "ESC to Exit", GRAY, disp.rgb(30, 60, 30))
    end,
```
Add `sfx.play("menu_select")` to the ENTER branch and `sfx.play("menu_move")` where relevant (menu has no cursor movement — ENTER/ESC only; just the select sound). `draw_garden_layers(0)` needs the function defined before `menu_scene` — it already is (section 6 precedes section 13).

- [ ] **Step 3: Sim verify**

Screenshot title; second screenshot 0.7 s later → pig position must differ (byte-diff). Read-through: logo/title legible, backdrop is the garden, footer panel tidy.

- [ ] **Step 4: Commit**

```bash
git add apps/guinea_pig/sprites/title_logo.png apps/guinea_pig/main.lua
git commit -m "guinea_pig: composed garden title screen with logo + running pig"
```

---

### Task 10: HUD polish

**Files:**
- Create (generated): `apps/guinea_pig/sprites/heart.png`
- Modify: `apps/guinea_pig/main.lua` (`draw_hud` ~lines 1360-1397)

**Interfaces:**
- Produces: none beyond the file (draw_hud stays self-contained).

- [ ] **Step 1: Generate the heart icon**

```
mcp__pixellab__create_map_object(description="shiny red heart icon, pixel art game HUD heart with white highlight", view="side", width=32, height=32, no_background=true)
```
Flatten → `sprites/heart.png`; `load_sprite("heart", "heart.png")`.

- [ ] **Step 2: Rewrite `draw_hud`**

Full replacement of the current `draw_hud` (~lines 1360-1397). Full hearts use the sprite; empty hearts stay procedural dark (no overdraw hacks):

```lua
local function draw_hud()
    disp.fillRect(0, 0, SCREEN_W, 18, BLACK)
    for i = 0, 2 do
        local hx = 4 + i * 18
        if i < player.hp then
            if sprites.heart then
                sprites.heart:drawScaled(hx, 1, 0.5)
            else
                disp.fillRect(hx, 3, 3, 3, RED) disp.fillRect(hx + 4, 3, 3, 3, RED)
                disp.fillRect(hx + 1, 5, 5, 4, RED) disp.fillRect(hx + 2, 9, 3, 2, RED)
            end
        else
            -- empty heart: dark outline, same footprint as the 16px sprite
            disp.drawRect(hx + 2, 3, 5, 5, DARK_GRAY) disp.drawRect(hx + 9, 3, 5, 5, DARK_GRAY)
            disp.fillRect(hx + 3, 8, 10, 3, DARK_GRAY) disp.fillRect(hx + 5, 11, 6, 3, DARK_GRAY)
        end
    end
    disp.drawText(62, 4, "" .. player.score, WHITE, BLACK)
    if sprites.carrot then
        sprites.carrot:drawScaled(104, 1, 0.5)
    end
    disp.drawText(122, 4, veggies_collected .. "/" .. total_veggies, GREEN, BLACK)
    -- Power indicators
    local ix = 168
    if player.zoomies_active then
        local bar = math.floor((player.zoomies_timer / ZOOMIES_DURATION) * 16)
        disp.fillRect(ix, 3, bar, 4, RED)
        disp.drawText(ix, 8, "SPD", RED, BLACK)
        ix = ix + 26
    end
    if player.nail_grip_active then
        local bar = math.floor((player.nail_grip_timer / NAIL_GRIP_DURATION) * 16)
        disp.fillRect(ix, 3, bar, 4, GRAY)
        disp.drawText(ix, 8, "GRP", GRAY, BLACK)
        ix = ix + 26
    end
    if player.has_shield then
        disp.fillCircle(ix + 4, 8, 4, ORANGE)
        ix = ix + 14
    end
    if player.inverted_controls then
        disp.drawText(ix, 4, "!?", MAGENTA, BLACK)
    end
end
```

- [ ] **Step 3: Sim verify + commit**

Screenshot HUD after taking damage (walk into the snail/hawk): full vs empty hearts distinct; carrot icon next to count.

```bash
git add apps/guinea_pig/sprites/heart.png apps/guinea_pig/main.lua
git commit -m "guinea_pig: HUD heart + veggie icons"
```

---

### Task 11: Win / game-over screens

**Files:**
- Modify: `apps/guinea_pig/main.lua` (`play_scene.draw` dead branch ~lines 1512-1518; `win_scene` ~lines 1523-1557)

**Interfaces:**
- Consumes: `pc.display.applyEffect("darken", factor)` (existing firmware API), garden layers (Task 7), house sprite, SFX (Task 5).

- [ ] **Step 1: Game-over panel**

Replace the `player.dead` block in `play_scene.draw`:

```lua
    if player.dead then
        pc.display.applyEffect("darken", 140)
        disp.fillRect(56, 116, 208, 88, BLACK)
        disp.drawRect(56, 116, 208, 88, RED)
        disp.drawText(116, 130, "GAME OVER", RED, BLACK)
        disp.drawText(100, 150, "Score: " .. player.score, WHITE, BLACK)
        disp.drawText(84, 168, "Veggies: " .. veggies_collected .. "/" .. total_veggies, GREEN, BLACK)
        disp.drawText(84, 186, "ENTER: Try Again", GRAY, BLACK)
    end
```
(The `game_over` SFX was already wired in Task 5 where `player.dead` is set.)

- [ ] **Step 2: Win scene**

Rewrite `win_scene.draw` to use the garden backdrop statically + house + stats panel:

```lua
    draw = function()
        draw_sky()
        draw_garden_layers(0)
        draw_house(128, 46)
        disp.fillRect(40, 116, 240, 120, disp.rgb(20, 40, 20))
        disp.drawRect(40, 116, 240, 120, GOLD)
        disp.drawText(72, 126, "HOME SWEET HOME!", GOLD, disp.rgb(20, 40, 20))
        disp.drawText(100, 146, "Score: " .. player.score, WHITE, disp.rgb(20, 40, 20))
        disp.drawText(84, 162, "Veggies: " .. veggies_collected .. "/" .. total_veggies, GREEN, disp.rgb(20, 40, 20))
        if veggies_collected >= total_veggies then
            disp.drawText(64, 178, "ALL VEGGIES! +500!", GOLD, disp.rgb(20, 40, 20))
        end
        if player.score >= high_score and player.score > 0 then
            disp.drawText(76, 196, "NEW HIGH SCORE!", YELLOW, disp.rgb(20, 40, 20))
        else
            disp.drawText(80, 196, "Best: " .. high_score, GRAY, disp.rgb(20, 40, 20))
        end
        disp.drawText(74, 216, "ENTER: Play Again", WHITE, disp.rgb(20, 40, 20))
        disp.drawText(92, 230, "ESC: Menu", GRAY, disp.rgb(20, 40, 20))
        draw_particles_at(0, 0)
    end
```

- [ ] **Step 3: Sim verify**

- Game-over: launch, walk into the hawk (stand at x≈1100 after ~60 right-presses, wait for dive ×3) → screenshot shows dimmed backdrop + panel.
- Win: TEMPORARY uncommitted tweak — in the staged SD copy only, edit `main.lua` win check `3050` → `150` so touching spawn area wins; screenshot; then re-push the clean app. Never commit the shortcut.

- [ ] **Step 4: Commit**

```bash
git add apps/guinea_pig/main.lua
git commit -m "guinea_pig: polished win + game-over screens"
```

---

### Task 12: Cleanup + full verification

**Files:**
- Delete: `apps/guinea_pig/sprites/mountain_far.png`, `mountain_mid.png`, `island_distant.png`, `hose_nozzle.png`, `ground_tiles.png`, `gp_idle.png`
- Modify: `apps/guinea_pig/app.json` (version → 1.1.0)
- Modify: `apps/guinea_pig/POLISH_DESIGN.md` (mark implemented; note plan-time refinements: discrete parallax clusters, composed title, win/game-over approach)

- [ ] **Step 1: Remove dead assets + any code refs**

`git rm` the files above; `grep -n "mountain\|island_distant\|hose_nozzle\|ground_tiles\|gp_idle\b" apps/guinea_pig/main.lua` must return nothing (gp_idle_east/west stay!).

- [ ] **Step 2: Full sim pass**

Fresh sim boot → push app → launch → screenshot title → play: run right ~200 presses with jumps, screenshot every ~40 presses (6 shots); confirm: no tile gaps, parallax coherent, sprinkler sweep visible in section 2, HUD icons, death panel (take hits), fallback-free log (`get_log_buffer` — no Lua errors, no `sound:` errors).

- [ ] **Step 3: `check_assets.py` final**

```bash
cd apps/guinea_pig && python3 tools/check_assets.py
```
Expected: `check_assets: all OK`.

- [ ] **Step 4: Bump version + docs**

`app.json` `"version": "1.1.0"`; update POLISH_DESIGN.md status section.

- [ ] **Step 5: Hardware pass (if device attached)**

Confirm with `mcp__picos__get_status` (hardware, not sim). Flash firmware with the Task 3 fixes (`make flash-ota` per project memory, from launcher — exit any running app first), `push_app` the polished game, launch, and listen: jump/collect/damage/win sounds distinct chiptune (not beeps), BGM audible in menu. Screenshot title + gameplay on device. If no device is attached, mark this step deferred-for-user and note it in the final summary.

- [ ] **Step 6: Commit**

```bash
git add -A apps/guinea_pig
git commit -m "guinea_pig: v1.1.0 — remove superseded assets, final polish pass"
```

---

## Self-Review Notes

- **Spec coverage:** tiles §2 → Task 2; parallax §Assets → Task 7; title → Task 9; sprinkler → Task 8; sounds §Sound engine → Tasks 4-6; firmware fixes → Task 3; HUD/win/game-over → Tasks 10-11; verification/cleanup → Task 12. BGM → Task 6.
- **Plan-time deviations from spec (flagged in Task steps):** discrete parallax clusters instead of wrapped strips; composed title instead of `title_bg.png`; `water_drop.png` dropped (procedural droplets); `veggie_icon.png` dropped (reuse carrot.png). If the user wants literal spec assets instead, regenerate per spec §Assets.
- **Known fragilities:** PixelLab grid layouts need the eyeball checkpoint (Task 2 Step 2); logo text may garble (Task 9 fallback); water animation may need static fallback (Task 8 Step 1); `pc.fs.exists` exact name to verify at implementation (Task 5).
