# Guinea Pig Platformer - Implementation Plan

## Context

Create a guinea pig platformer for PicOS where the player collects vegetables (carrots, capsicum, zucchini), avoids/defeats enemies, uses special abilities, and reaches a dome-shaped guinea pig house at the end. Features parallax scrolling clouds and a single level. Based on `apps/platformer_demo/main.lua` patterns.

## App Structure

```
apps/guinea_pig/
  app.json          -- App manifest (ALREADY CREATED)
  main.lua          -- All game code (~1000-1200 lines)
  sprites/          -- PixelLab-generated PNG sprites
```

## Step 1: Generate Sprites with PixelLab MCP

Generate pixel art sprites as PNG files in `apps/guinea_pig/sprites/`. Magenta (255,0,255) background for transparency. Use the PixelLab MCP tools to generate each asset.

| Asset | Size | Description |
|-------|------|-------------|
| `guinea_pig.png` | 96x32 spritesheet (4x 24x16) | idle, run1, run2, jump frames. Cute side-view, brown/white fur, small ears, dot eyes, tiny legs |
| `carrot.png` | 8x14 | Orange carrot with green leafy top |
| `capsicum.png` | 10x12 | Red bell pepper with green stem |
| `zucchini.png` | 12x8 | Green zucchini |
| `house.png` | 48x36 | Red/orange dome-shaped plastic guinea pig igloo house with dark entrance hole (reference image: dome pet house) |
| `ground.png` | 32x16 | Grass-topped brown earth tile, seamless horizontal |
| `cloud_small.png` | 32x12 | Small fluffy white cloud |
| `cloud_large.png` | 48x16 | Large fluffy white cloud |
| `hawk.png` | 28x16 | Dark bird with spread wings, menacing |
| `snail.png` | 16x12 | Snail with brown spiral shell |
| `broccoli.png` | 14x18 | Green broccoli with thick stem, leafy top, angry face |
| `spicy_pepper.png` | 10x14 | Red chili pepper with angry face |
| `hose.png` | 16x16 | Green garden hose nozzle |
| `hay_pile.png` | 32x20 | Golden hay pile, fluffy |
| `strawberry.png` | 10x10 | Red strawberry with seeds |
| `vitamin_c.png` | 12x12 | Orange glowing orb |
| `fireball.png` | 6x6 | Small flame seed projectile |
| `water_spray.png` | 48x8 | Horizontal blue water stream |

**Fallback:** If PixelLab unavailable, draw all sprites procedurally with `fillRect`/`fillCircle` calls.

## Step 2: Create app.json (DONE)

Already created at `apps/guinea_pig/app.json`:
```json
{
  "id": "com.picos.guineapig",
  "name": "Guinea Pig Run",
  "description": "Collect veggies, avoid enemies, and find your way home!",
  "version": "1.0.0",
  "author": "PicOS",
  "requirements": ["audio"]
}
```

## Step 3: Game Code — Constants & Player State

```lua
-- Physics
GRAVITY = 800, JUMP_POWER = -380, RUN_SPEED = 150, FRICTION = 0.8
DASH_SPEED = 350, ZOOMIES_SPEED = 400, ZOOMIES_DURATION = 3.0
POPCORN_JUMP_POWER = -300, POPCORN_KNOCKBACK_RADIUS = 40
WORLD_WIDTH = 3200, WORLD_HEIGHT = 320, GROUND_Y = 280

-- Player state
player = {
  x, y, vx, vy, w=24, h=16,
  on_ground, facing, anim_timer, anim_frame,
  hp = 3,                    -- 3 hits total
  score = 0,
  hiding = false,            -- in hay pile
  has_shield = false,        -- vitamin C active
  shield_timer = 0,
  zoomies_timer = 0,         -- speed boost remaining
  zoomies_active = false,
  nail_grip_timer = 0,       -- wall climb remaining
  can_popcorn = true,        -- double jump available (resets on ground)
  dash_cooldown = 0,         -- dash attack cooldown
  stunned_timer = 0,         -- spicy pepper disorientation
}
```

## Step 4: Enemies

### 4a. Hawk (Dive-Bomber)
- **Behavior**: Patrols at top of screen (y=20). When player is within x-range and NOT hiding, swoops diagonally toward player at high speed. Returns to patrol height after reaching ground level.
- **Data**: `{x, y, patrol_x1, patrol_x2, state="patrol"|"diving"|"rising", target_x, target_y, timer}`
- **Avoidance**: Player must duck into hay piles or hide under covered platforms. If hiding=true, hawk aborts dive.
- **Damage**: 1 HP on contact during dive. Knocks player back.
- **Placement**: 2 hawks in level (sections 3 and 5)

### 4b. Garden Hose (Hazard)
- **Behavior**: Fixed position on ground. Periodically activates (2s on, 3s off). Sprays horizontal water stream across ~48px.
- **Data**: `{x, y, direction=1|-1, timer, active, spray_w=48}`
- **Effect**: Pushes player in spray direction at 200px/s. No HP damage.
- **Visual**: Green hose nozzle + blue animated water rectangles when active
- **Placement**: 2 hoses in level (sections 2 and 4)

### 4c. Snail (Tank)
- **Behavior**: Slow patrol (30px/s) on platforms, reverses at edges. Cannot be jumped on (shell blocks).
- **Data**: `{x, y, vx=30, w=16, h=12, flipped=false, flip_timer}`
- **Defeat**: Dash-attack to flip over (becomes harmless, slides off platform). OR lure to platform edge (falls off).
- **Damage**: 1 HP on contact (unless flipped).
- **Placement**: 2 snails (sections 3 and 4)

### 4d. Sentient Broccoli (Shielded)
- **Behavior**: Stationary. Leafy top blocks overhead jumps (player bounces off). Vulnerable stem at base.
- **Data**: `{x, y, w=14, h=18, alive=true, hit_timer}`
- **Defeat**: Dash-attack into stem (lower half hitbox). 1 dash = defeat.
- **Damage**: 1 HP on contact from sides. Jump on top = bounce off (no damage to either).
- **Placement**: 2 broccoli (sections 3 and 5)

### 4e. Spicy Pepper (Ranged)
- **Behavior**: Stationary. Fires small fireball seeds every 2s toward player.
- **Data**: `{x, y, w=10, h=14, alive=true, fire_timer}`
- **Projectiles**: `{x, y, vx, vy}` — travel at 150px/s toward player, gravity-affected. Despawn off-screen.
- **Defeat**: Dash-attack. If player touches pepper body (not fireball), triggers "eaten" effect: 4s of double speed + inverted left/right controls.
- **Damage**: Fireball = 1 HP.
- **Placement**: 2 peppers (sections 4 and 5)

## Step 5: Abilities

### 5a. Pop-corn Jump (innate)
- **Trigger**: Press jump while airborne (double jump). Usable once per air sequence, resets on landing.
- **Effect**: Small upward boost (POPCORN_JUMP_POWER = -300). Emits sparkle particles in a ring. Knocks back nearby light enemies within 40px radius.
- **Visual**: Burst of yellow/white particles + brief screen flash

### 5b. Hiding in Hay (environmental)
- **Objects**: 4-5 hay piles placed throughout level, especially near hawk patrol zones
- **Trigger**: Press DOWN while overlapping hay pile -> player.hiding = true, sprite hidden
- **Effect**: Invisible to hawks (abort dive), immune to fireballs. Cannot move while hiding.
- **Exit**: Release DOWN or press LEFT/RIGHT
- **Visual**: Hay pile wiggles slightly when player is inside

### 5c. Nail Grip (power-up pickup)
- **Pickup**: Sparkly claw icon, 1-2 in level near vertical surfaces
- **Effect**: 5 second timer. While active, pressing UP against a wall makes player climb vertically at 100px/s. Player sticks to wall (no gravity) while holding direction toward it.
- **Visual**: Small claw sparks on player sprite, timer bar in HUD

### 5d. Zoomies (strawberry pickup)
- **Pickup**: Strawberry, 1-2 in level
- **Effect**: 3 second speed boost (ZOOMIES_SPEED = 400px/s). Can cross small water/gap hazards. Motion blur trail (3-4 afterimage rectangles behind player with decreasing opacity).
- **Visual**: Afterimage trail, speed lines particles

### 5e. Vitamin C Shield (pickup)
- **Pickup**: Orange orb, 2 in level
- **Effect**: Absorbs next hit (any source). Shatters with particle burst on hit. Lasts until hit or level end.
- **Visual**: Translucent orange circle drawn around player (flickering fillCircle every other frame). Shatter = orange particles.

### 5f. Sonic Squeak (charged ability, innate)
- **Trigger**: Hold F1 for 1s to charge -> release to fire
- **Effect**: Horizontal sound wave projectile (toward facing direction). Pushes back light enemies 60px, breaks fragile barriers (glass jar obstacles), stuns snails for 2s.
- **Visual**: Expanding semicircle wave in yellow/white, "WHEEK!" text popup
- **Cooldown**: 5 seconds

## Step 6: Level Design (3200px wide)

```
Section 1: Tutorial (0-500)
  Ground: flat, x=0 to x=500
  Items: 2 carrots, 1 hay pile
  Teach: movement, jump, collection

Section 2: First Challenges (500-900)
  Ground: gap at x=600 (48px), platform above
  Items: 1 capsicum on platform, 1 zucchini
  Enemy: 1 garden hose at x=750 (sprays right)
  Teach: jumping gaps, avoiding hose pushback

Section 3: Vertical Challenge (900-1400)
  Ground: ends at x=950, resumes at x=1350
  Platforms: 4 floating, staircase pattern ascending
  Items: 2 carrots, 1 capsicum, 1 strawberry (zoomies)
  Enemies: 1 hawk patrol (x=900-1400), 1 snail on wide platform, 1 broccoli
  Cover: 1 hay pile under covered platform
  Teach: popcorn jump, hay hiding, dash-attack

Section 4: The Gauntlet (1400-2000)
  Ground: broken segments with gaps
  Platforms: 5 mixed heights
  Items: 2 zucchini, 1 carrot, 1 vitamin C shield, 1 nail grip
  Enemies: 1 garden hose, 1 snail, 1 spicy pepper
  Vertical wall: climbable surface (for nail grip) at x=1800
  Teach: nail grip, shield, pepper danger

Section 5: Homestretch (2000-2700)
  Ground: mostly flat with 2 small gaps
  Items: 1 capsicum, 1 zucchini, 1 strawberry
  Enemies: 1 hawk, 1 broccoli, 1 spicy pepper
  Cover: 1 hay pile
  Glass barrier: breakable with sonic squeak at x=2500
  Teach: sonic squeak, zoomies dash

Section 6: Home (2700-3200)
  Ground: flat, safe
  Items: 1 final carrot right before house
  House: at x=3050 on ground
  No enemies — peaceful arrival
```

**Total collectibles**: ~15 veggies + 2 strawberries + 2 vitamin C + 1-2 nail grips

## Step 7: Parallax Background

Draw back-to-front in `draw()`:
```
Layer 0: Sky gradient (static)              — 0.0x scroll
Layer 1: Distant hills (dark green)         — 0.15x scroll
Layer 2: Far clouds                         — 0.25x scroll
Layer 3: Near clouds                        — 0.5x scroll
Layer 4: World (ground, platforms, enemies, items, player) — 1.0x
Layer 5: HUD                               — 0.0x (screen-fixed)
```
Formula: `parallax_ox = ox * factor + 160 * (1 - factor)`

## Step 8: HUD

Top bar (y=0-20, black background):
- Left: `HP: ♥♥♥` (red hearts, draw as small red fillRect blocks)
- Center: Score
- Right: Veggie count `7/15`
- Active power-up indicators (shield icon, timer bars for nail grip/zoomies)

## Step 9: Scenes

**Menu**: Title "GUINEA PIG RUN", guinea pig sprite, instructions (arrows=move, up/enter=jump, F1=squeak, down=hide), high score, ENTER to start, ESC to exit
**Play**: Full gameplay with all systems
**Win**: "HOME SWEET HOME!", score, veggie count, bonus for all collected, high score save

## Step 10: Sounds

| Event | Freq | Duration |
|-------|------|----------|
| Jump | 440Hz | 80ms |
| Popcorn jump | 660Hz+880Hz | 60ms each |
| Collect carrot | 880Hz | 100ms |
| Collect capsicum | 660Hz | 100ms |
| Collect zucchini | 550Hz | 100ms |
| Collect power-up | 440->880Hz | 80ms each |
| Dash attack | 330Hz | 60ms |
| Sonic squeak | 1200Hz->600Hz | 200ms |
| Take damage | 220Hz | 150ms |
| Hawk screech | 1000Hz | 120ms |
| Win | 440->660->880Hz | 150ms each |

## Implementation Order

1. `app.json` (DONE) + main.lua skeleton (main loop, scene registration)
2. Sprite generation with PixelLab MCP (or procedural fallback)
3. Menu scene with title
4. Play scene: ground, player physics, camera, parallax background
5. Platforms and collision system
6. Collectibles (veggies) with particles and sounds
7. Guinea pig house and win condition + win scene
8. Enemies: hose (simplest) -> snail -> broccoli -> pepper -> hawk
9. Abilities: popcorn jump -> hiding -> dash-attack -> shield -> zoomies -> nail grip -> sonic squeak
10. HUD with HP, score, power-up indicators
11. Polish: tune physics, level layout, difficulty curve

## Key Reference Files
- `apps/platformer_demo/main.lua` — scene/camera/collision/particle patterns to reuse
- `src/os/lua_bridge_graphics.c` — image API (draw with flipX, subrect for spritesheets)
- `src/os/lua_bridge_game_camera.c` — camera getOffset() for parallax math
- `src/os/os.h` — BTN_* constants, display API struct

## Key PicOS API Patterns (from exploration)

### Game Loop
```lua
while true do
    pc.perf.beginFrame()
    input.update()
    local dt = pc.perf.getFrameTime() / 1000.0
    if dt > 0.05 then dt = 0.05 end
    game.scene.update(dt)
    game.scene.draw()
    pc.display.flush()
    pc.perf.endFrame()
    pc.sys.sleep(16)
end
```

### Image Loading
```lua
local img = picocalc.graphics.image.load(APP_DIR .. "/sprites/guinea_pig.png")
img:setTransparentColor(disp.rgb(255, 0, 255))  -- magenta = transparent
img:draw(x, y, {flipX = facing < 0}, {x = frame * 24, y = 0, w = 24, h = 16})  -- subrect for spritesheet
```

### Camera
```lua
local camera = game.camera.new()
camera:setBounds(0, 0, WORLD_WIDTH, WORLD_HEIGHT)
camera:setTarget(player.x, player.y - 40, 0.1)  -- smooth follow
camera:update(dt)
local ox, oy = camera:getOffset()
-- Draw world objects at (obj.x + ox, obj.y + oy)
```

### Collision (AABB)
```lua
local function aabb_overlap(ax, ay, aw, ah, bx, by, bw, bh)
    return ax < bx + bw and ax + aw > bx and ay < by + bh and ay + ah > by
end
```

### Input
```lua
local pressed = input.getButtonsPressed()
local held = input.getButtons()
if pressed & input.BTN_UP ~= 0 then ... end    -- edge detect
if held & input.BTN_LEFT ~= 0 then ... end      -- continuous hold
```

### Scene System
```lua
game.scene.add("menu", { enter=fn, exit=fn, update=fn(dt), draw=fn })
game.scene.switch("play")
```

## Verification
1. Use PicOS simulator/MCP tools:
   - `mcp__picos__launch_app` to launch "Guinea Pig Run"
   - `mcp__picos__screenshot` to verify visual output
   - `mcp__picos__keypress` to test controls
2. Test each system: movement -> jumping -> popcorn jump -> collecting -> enemy interactions -> abilities -> parallax -> win condition -> score saving
