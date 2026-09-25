-- panels_demo/comic.lua — the comic data table.
--
-- This file doubles as living documentation for the panels.lua schema: each
-- sequence exercises one feature group. Pure data plus tiny render callbacks;
-- all assets resolve via the source (images/ and audio/ under APP_DIR).

local pc    = picocalc
local disp  = pc.display
local input = pc.input

local white  = disp.rgb(235, 235, 235)
local yellow = disp.rgb(255, 210, 80)

return {
    name = "Panels Demo",

    sequences = {

        -- 1 ── Vertical scroll with three-layer parallax.
        -- Layer art is taller than the panels, so the clip rect visibly crops
        -- overhang at panel borders while layers slide at different rates.
        {
            title = "City",
            scrollType = "scroll",
            panels = {
                {
                    frame = { height = 480 },
                    layers = {
                        { image = "s1_sky.png",  x = 0, y = -110, parallax = 0.15 },
                        { image = "s1_city.png", x = 0, y = -150, parallax = 0.5,
                          transparentColor = true },
                        { image = "s1_rail.png", x = 0, y = -120, parallax = 0.9,
                          transparentColor = true },
                        { text = "The city never sleeps.", x = 20, y = 40,
                          parallax = 0 },
                    },
                },
                {
                    frame = { height = 420, marginBefore = 8 },
                    layers = {
                        { image = "s1_sky.png",  x = 0, y = -160, parallax = 0.2 },
                        { image = "s1_city.png", x = 0, y = -140, parallax = 0.65,
                          transparentColor = true },
                        { text = "Neither do I.", x = 100, y = 200, parallax = 0,
                          scrollTrigger = 0.45 },
                    },
                },
                {
                    frame = { height = 380, marginBefore = 8 },
                    layers = {
                        { image = "s1_sky.png", x = 0, y = -220, parallax = 0.3 },
                        { text = "Tonight it ends.", x = 80, y = 180,
                          scrollTrigger = 0.5 },
                    },
                },
            },
        },

        -- 2 ── Rigid vertical scroll: no parallax, no animation, no image
        -- arrays — eligible for the LCD hardware-scroll fast path.  panels.lua
        -- lays the sequence into the panel's frame memory as a mod-320 ring
        -- and scrolls by register write; only newly revealed strips are drawn.
        -- Compare the feel with sequence 1, which parallax forces to redraw
        -- three full-screen layers every frame.
        {
            title = "The Descent",
            scrollType = "scroll",
            panels = {
                {
                    frame = { height = 420 },
                    layers = {
                        { image = "s1_sky.png", y = -40 },
                        { text = "Down the long stairwell.", x = 30, y = 40 },
                        { text = "(hardware scroll - hold DOWN)", x = 30, y = 60 },
                    },
                },
                {
                    frame = { height = 420, marginBefore = 8 },
                    layers = {
                        { image = "s1_city.png", y = -20, transparentColor = true },
                        { text = "Every floor darker.", x = 60, y = 320 },
                    },
                },
                {
                    frame = { height = 420, marginBefore = 8 },
                    layers = {
                        { image = "s1_rail.png", y = 0, transparentColor = true },
                        { text = "Every step quieter.", x = 90, y = 60 },
                    },
                },
                {
                    frame = { height = 420, marginBefore = 8 },
                    layers = {
                        { image = "s5_stars.png", y = -60 },
                        { text = "Until the street.", x = 100, y = 200 },
                    },
                },
            },
        },

        -- 3 ── Panel-by-panel advance with a keyframed slide and a blink.
        {
            title = "The Meeting",
            scrollType = "advance",
            advanceControl = input.BTN_RIGHT,
            panels = {
                {
                    frame = { height = 320 },
                    layers = {
                        { image = "s2_bg.png" },
                        -- Hero slides in from off-panel once the panel lands.
                        { image = "s2_hero.png", x = -80, y = 190,
                          transparentColor = true,
                          animate = { x = 60, duration = 700, ease = "cubicOut" } },
                    },
                },
                {
                    frame = { height = 320, marginBefore = 8 },
                    effect = { type = "shake", strength = 2 },
                    layers = {
                        { image = "s2_bg.png" },
                        { image = "s2_hero.png", x = 60, y = 190,
                          transparentColor = true },
                        { image = "s2_alert.png", x = 190, y = 60,
                          transparentColor = true,
                          effect = { type = "blink", onMs = 350, offMs = 250 } },
                        { text = "!!", x = 230, y = 130, color = yellow },
                    },
                },
            },
        },

        -- 4 ── Auto-advance, an image frame-array, sfx trigger and looping bgm.
        {
            title = "Dawn",
            scrollType = "auto",
            autoAdvanceMs = 2200,
            audio = { file = "bgm.wav", loop = true },
            panels = {
                {
                    frame = { height = 360 },
                    audio = { file = "chime.wav", scrollTrigger = 0.3 },
                    layers = {
                        -- Frame selection follows scroll percentage; under
                        -- auto-advance that reads as a slow sunrise.
                        { images = { "s3_dawn1.png", "s3_dawn2.png",
                                     "s3_dawn3.png" } },
                        { text = "Morning came anyway.", x = 40, y = 30 },
                    },
                },
                {
                    frame = { height = 360, marginBefore = 8 },
                    layers = {
                        { image = "s3_dawn3.png" },
                        { text = "Decision time.", x = 90, y = 40 },
                    },
                },
            },
        },

        -- 5 ── Branching choice; sets a story var the finale reacts to.
        {
            title = "The Choice",
            scrollType = "advance",
            panels = {
                {
                    frame = { height = 320 },
                    layers = {
                        { image = "s2_bg.png" },
                        { text = "Stay or go?", x = 100, y = 40, color = yellow },
                    },
                    choices = {
                        prompt = "What now?",
                        { text = "Stay and fight", target = 6,
                          setVars = { stayed = true } },
                        { text = "Walk away",      target = 6,
                          setVars = { stayed = false } },
                    },
                },
            },
        },

        -- 6 ── renderCondition payoff + fade transition into credits.
        {
            title = "Epilogue",
            scrollType = "advance",
            transition = "fadeToBlack",
            panels = {
                {
                    frame = { height = 320 },
                    layers = {
                        { image = "s5_stars.png", y = -20, parallax = 0.2 },
                        { text = "You stayed. The city remembers.",
                          x = 30, y = 140,
                          renderCondition = { var = "stayed", equals = true } },
                        { text = "You walked. The road remembers.",
                          x = 30, y = 140,
                          renderCondition = { var = "stayed", equals = false } },
                    },
                },
            },
        },

        -- 7 ── Credits: text layers over a slow starfield, custom render demo.
        {
            title = "Credits",
            scrollType = "scroll",
            backgroundColor = disp.rgb(4, 4, 12),
            panels = {
                {
                    frame = { height = 620 },
                    borderless = true,
                    layers = {
                        { image = "s5_stars.png", y = -130, parallax = 0.25 },
                        { text = "PANELS DEMO", x = 110, y = 60, color = yellow },
                        { text = "story  - the framework", x = 60, y = 120 },
                        { text = "art    - gen_assets.py", x = 60, y = 140 },
                        { text = "music  - three sines in a trenchcoat",
                          x = 60, y = 160 },
                        { text = "Press ESC to exit", x = 90, y = 300,
                          scrollTrigger = 0.55, color = white },
                    },
                    -- Custom render callback: a pulsing underline, drawn with
                    -- the panel clip still active.
                    renderFunction = function(panel, ox, oy, pct)
                        local w = math.floor(80 + 60 * pct)
                        disp.fillRect(ox + 110, oy + 76, w, 2, yellow)
                    end,
                },
            },
        },
    },
}
