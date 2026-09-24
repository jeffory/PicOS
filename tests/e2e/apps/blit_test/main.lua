-- Blit Test — fixture for the display blitters.
--
-- Lua cannot read pixels back, so each phase only draws; the harness probes
-- with get_pixel and compares against the PNGs it decodes itself. Each ENTER
-- advances one phase.

local pc    = picocalc
local disp  = pc.display
local gfx   = pc.graphics
local input = pc.input
local now   = pc.sys.getTimeMs

local BG    = disp.rgb(0, 0, 80)       -- backdrop
local KEY   = disp.rgb(248, 0, 248)    -- sprite colour key (magenta)

local pattern = gfx.image.load(APP_DIR .. "/pattern.png")  -- 64x48
local sprite  = gfx.image.load(APP_DIR .. "/sprite.png")   -- 8x6, KEY background
sprite:setTransparentColor(KEY)
pc.sys.log("BT:LOADED")

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
