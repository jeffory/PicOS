-- Blit Test — fixture for the display blitters and off-screen shape rejection.
--
-- Lua cannot read pixels back, so each phase only draws; the harness probes
-- with get_pixel and compares against the PNGs it decodes itself. Each ENTER
-- advances one phase. Phases that must return promptly (huge off-screen
-- shapes, a 3D vertex behind the camera) log their elapsed milliseconds as
-- "BT:MS <name> <ms>" — before the fix they looped for billions of pixels.

local pc    = picocalc
local disp  = pc.display
local gfx   = pc.graphics
local input = pc.input
local now   = pc.sys.getTimeMs

local BG    = disp.rgb(0, 0, 80)       -- backdrop
local INK   = disp.rgb(0, 255, 0)      -- line / shape colour
local KEY   = disp.rgb(248, 0, 248)    -- sprite colour key (magenta)

local pattern = gfx.image.load(APP_DIR .. "/pattern.png")  -- 64x48
local sprite  = gfx.image.load(APP_DIR .. "/sprite.png")   -- 8x6, KEY background
sprite:setTransparentColor(KEY)
pc.sys.log("BT:LOADED")

local function timed(name, fn)
    local t0 = now()
    fn()
    pc.sys.log(("BT:MS %s %d"):format(name, now() - t0))
end

local phases = {
    -- A real PNG through PNGdec's inflate (the unaligned-copy path), drawn
    -- opaque at the origin and again at a negative offset (clipped top/left).
    { name = "png", draw = function()
        disp.clear(BG)
        pattern:draw(0, 0)
        pattern:draw(-5, 100 - 3)          -- 5 columns and 3 rows clipped
        pattern:draw(300, 250)             -- clipped right and bottom
    end },

    -- Colour-keyed sprite, unflipped / flipX / flipY / both, each at a known
    -- spot; KEY pixels must leave BG untouched.
    { name = "keyed_flip", draw = function()
        disp.clear(BG)
        sprite:draw(16, 16)
        sprite:draw(32, 16, true)
        sprite:draw(48, 16, { flipY = true })
        sprite:draw(64, 16, { flipX = true, flipY = true })
        sprite:draw(-3, 40, true)           -- flipped and clipped on the left
        sprite:draw(316, 40, { flipY = true }) -- clipped on the right
        sprite:draw(16, -2, { flipX = true })  -- clipped on the top
    end },

    -- Integer nearest-neighbour scale with negative, non-multiple offsets.
    { name = "nn_negative", draw = function()
        disp.clear(BG)
        sprite:drawScaledNN(-3, -5, 4)      -- 32x24 dst, starts off-screen
        sprite:drawScaledNN(300, 310, 3)    -- clipped right and bottom
    end },

    { name = "line_huge", draw = function()
        disp.clear(BG)
        timed("line_huge", function()
            disp.drawLine(0, 0, 1000000000, 1000000000, INK)
            disp.drawLine(-1000000000, 50, 1000000000, 50, INK)
            disp.drawLine(1000000000, -1000000000, 1000000001, 1000000000, INK)
        end)
    end },

    -- A circle of radius 1e9 whose top edge grazes y=60 (outline) and a filled
    -- one whose top edge is at y=200: both touch the screen, so neither can be
    -- rejected outright, and the naive loops run ~1e9 iterations.
    { name = "circle_huge", draw = function()
        disp.clear(BG)
        timed("circle_huge", function()
            disp.drawCircle(160, 1000000060, 1000000000, INK)
            disp.fillCircle(160, 1000000200, 1000000000, INK)
            disp.fillCircle(-2000000000, -2000000000, 5, INK)  -- far off-screen
        end)
    end },

    { name = "tri_huge", draw = function()
        disp.clear(BG)
        timed("tri_huge", function()
            disp.fillTriangle(0, 100, 1000000000, 100, 0, 1000000000, INK)
        end)
    end },

    -- A cube with a vertex on the camera plane and one behind it: the
    -- projection divided by zero / flipped sign, which drew wild lines or
    -- converted inf to int (undefined) and spun in drawLine.
    { name = "cube_behind", draw = function()
        disp.clear(BG)
        -- fov = 100 puts the camera at z = -100: vertex 9 sits exactly on it
        -- (fov + z == 0, a divide by zero) and vertex 10 is behind it.
        local v = { -40,-40,-40,  40,-40,-40,  40,40,-40,  -40,40,-40,
                    -40,-40,40,   40,-40,40,   40,40,40,   -40,40,40,
                    0,0,-100,     10,10,-300 }
        local e = { 1,2, 2,3, 3,4, 4,1, 5,6, 6,7, 7,8, 8,5, 1,5, 2,6, 3,7, 4,8,
                    1,9, 2,9, 3,9, 4,9, 7,10 }
        local f = { 1,2,9, 2,3,9, 3,4,9, 4,1,9 }
        timed("cube_behind", function()
            draw3DWireframeEx(v, e, 0, 0, 0, 160, 160, 100, INK, INK, 2, 3, f)
        end)
    end },

    -- Blit throughput (measurement only; the simulator is not representative
    -- of device speed). Logged as BT:MS perf_<name> <ms>.
    { name = "perf", draw = function()
        disp.clear(BG)
        timed("perf_opaque", function()
            for i = 1, 20000 do pattern:draw((i * 7) % 300 - 20, (i * 13) % 300 - 20) end
        end)
        timed("perf_keyed_flip", function()
            for i = 1, 200000 do
                sprite:draw((i * 7) % 330 - 5, (i * 11) % 330 - 5, i % 2 == 0)
            end
        end)
        timed("perf_scaled", function()
            for i = 1, 20000 do sprite:drawScaledNN((i * 3) % 300 - 10, (i * 5) % 300 - 10, 3) end
        end)
        timed("perf_text", function()
            for i = 1, 20000 do disp.drawText((i * 3) % 300 - 30, (i * 7) % 330 - 5, "Hello, blitter!", INK, BG) end
        end)
    end },
}

local idx = 1

local function draw_phase()
    phases[idx].draw()
    disp.flush()
    pc.sys.log(("BT:PHASE %d %s"):format(idx, phases[idx].name))
end

draw_phase()

while true do
    input.update()
    local pressed = input.getButtonsPressed()
    if pressed & input.BTN_ESC ~= 0 then
        pc.sys.log("BT:DONE")
        return
    end
    if pressed & input.BTN_ENTER ~= 0 then
        idx = idx + 1
        if idx > #phases then
            pc.sys.log("BT:DONE")
            return
        end
        draw_phase()
    end
    pc.sys.sleep(16)
end
