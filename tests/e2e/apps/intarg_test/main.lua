-- Integer Args Test — fixture for tolerant integer bridge arguments.
--
-- lua_Number is float32, so ordinary app arithmetic yields values like
-- 199.99998 where double arithmetic landed on 200. Bridge parameters that are
-- quantities (coordinates, sizes, durations, volumes) must round such values
-- to nearest instead of raising "number has no integer representation", and
-- must still reject NaN, infinities and floats too large to hold an exact
-- integer (1e9) with a clean argument error rather than wrapping.
--
-- Logs one "IA <NAME> <value>" line per probe, then draws a fixed scene the
-- harness checks with get_pixel, then logs "IA DONE".

local pc    = picocalc
local disp  = pc.display
local gfx   = pc.graphics

-- Lines are also saved to /data/<APP_ID>/ia.txt at the end so a hardware
-- run (whose serial capture can drop lines) can be diffed against the sim.
local lines = {}
local function log(name, value)
    local line = "IA " .. name .. " " .. tostring(value)
    lines[#lines + 1] = line
    pc.sys.log(line)
end

-- Runs fn under pcall and reports "ok" or the error message.
local function try(name, fn)
    local ok, err = pcall(fn)
    log(name, ok and "ok" or ("err " .. tostring(err)))
end

-- ── Size: image.new(w, h) ────────────────────────────────────────────────
local ok, a = pcall(gfx.image.new, 10.5, 4.4)
if ok then
    local w, h = a:getSize()
    log("SIZE", w .. "x" .. h)                 -- expect 11x4
else
    log("SIZE", "err " .. tostring(a))
    a = nil
end
try("SIZE_BIG", function() gfx.image.new(1e9, 1) end)

-- ── Duration: animator duration + time ──────────────────────────────────
local okd, errd = pcall(function()
    local an = gfx.animation.animator.new(99.99999, 0, 100)
    log("DURATION", an:valueAtTime(50.5))   -- t = 51 of 100 ms -> 51.0
end)
if not okd then log("DURATION", "err " .. tostring(errd)) end
try("DURATION_BIG", function() gfx.animation.animator.new(1e9, 0, 1) end)
try("SLEEP", function() pc.sys.sleep(10.5) end)
try("REPEAT", function() pc.input.setRepeat(299.99998, 80.4) end)
pc.input.setRepeat(0)

-- ── Volume: modplayer set/getVolume ─────────────────────────────────────
local mp = assert(pc.modplayer.create())
mp:setVolume(50)                            -- known start (default is 100)
try("VOL_SET", function() mp:setVolume(99.99999) end)
log("VOL_A", mp:getVolume())                -- expect 100
pcall(function() mp:setVolume(10.5) end)
log("VOL_B", mp:getVolume())                -- expect 11
pcall(function() mp:setVolume(-0.4) end)
log("VOL_C", mp:getVolume())                -- expect 0
try("VOL_BIG", function() mp:setVolume(1e9) end)

-- ── Coordinate: image:draw(x, y) ────────────────────────────────────────
local b = gfx.image.new(2, 2)
try("COORD_BIG", function() b:draw(1e9, 0) end)
try("COORD_NAN", function() b:draw(0/0, 0) end)
try("COORD_INF", function() b:draw(math.huge, 0) end)
try("COORD_NEGINF", function() b:draw(-math.huge, 0) end)
try("COORD_STR", function() b:draw("abc", 0) end)
try("COORD_NUMSTR", function() b:draw("10.5", 0) end)
try("COORD_INT", function() b:draw(1000000000, 0) end)   -- exact integer: clipped, no error

-- ── Narrow sinks clamp instead of wrapping (Task 30) ─────────────────────
-- A uint8_t volume: 300 used to wrap to 44 and -1 to 255.
local okp, sp = pcall(pc.sound.sampleplayer)
if okp and sp then
    pcall(function() sp:setVolume(300) end)
    log("SVOL_300", sp:getVolume())             -- clamped to 0-100
    pcall(function() sp:setVolume(-1) end)
    log("SVOL_NEG", sp:getVolume())             -- expect 0
else
    log("SVOL_300", "err " .. tostring(sp)); log("SVOL_NEG", "err")
end
-- rgb components clamp to 0..255 before packing (256 used to wrap to 0)
log("RGB_HI", disp.rgb(255.7, 0, 0))            -- expect 63488 (0xF800)
log("RGB_NEG", disp.rgb(-3, 300, 0))            -- expect 2016 (0x07E0)
-- unsigned time: a negative time clamps to 0 instead of wrapping to ~4e9
local okt, errt = pcall(function()
    local an = gfx.animation.animator.new(100, 0, 100)
    log("VAT_NEG", an:valueAtTime(-1.5))        -- expect 0 (start value)
end)
if not okt then log("VAT_NEG", "err " .. tostring(errt)) end
-- display primitives reject NaN/inf like every other quantity argument
try("FILL_NAN", function() disp.fillRect(0/0, 0, 1, 1, 0) end)
try("TEXT_INF", function() disp.drawText(math.huge, 0, "x", 0) end)
-- table-field and array-entry errors name the argument and the field
try("FIELD_ERR", function()
    gfx.sprite.querySpritesAlongLine({ x1 = 0, y1 = 0/0, x2 = 1, y2 = 1 })
end)
try("ENTRY_ERR", function() pc.audio.pushSamples({ 1, 0/0 }) end)
-- a numeric string converts through a float, so beyond 2^24 it errors
try("NUMSTR_BIG", function() b:draw("20000000", 0) end)

-- ── Scene for the pixel probes ──────────────────────────────────────────
local WHITE = disp.rgb(255, 255, 255)
disp.clear(WHITE)
if a then pcall(function() a:draw(99.99999, 50) end) end        -- x 100..110, y 50..53
pcall(function() b:draw(10.5, 70.5) end)                        -- x 11..12, y 71..72
pcall(function() b:draw(-0.4, 90) end)                          -- x 0..1,   y 90..91
-- negative ties round toward +inf: -1.5 -> -1, -0.5 -> 0
pcall(function() b:draw(-1.5, 100) end)                         -- x -1..0,  y 100..101
pcall(function() b:draw(-0.5, 104) end)                         -- x 0..1,   y 104..105
-- display primitives round like img:draw: both 10.5s land on x = 11
pcall(function() disp.fillRect(10.5, 110, 1, 2, 0) end)         -- x 11,     y 110..111
pcall(function() b:draw(10.5, 114) end)                         -- x 11..12, y 114..115
disp.flush()

log("DONE", "1")
local path = pc.fs.appPath("ia.txt")
local f = path and pc.fs.open(path, "w")
if f then
    pc.fs.write(f, table.concat(lines, "\n") .. "\n")
    pc.fs.close(f)
end

while true do
    pc.input.update()
    if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end
    pc.sys.sleep(20)
end
