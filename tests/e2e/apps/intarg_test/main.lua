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

local function log(name, value)
    pc.sys.log("IA " .. name .. " " .. tostring(value))
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

-- ── Scene for the pixel probes ──────────────────────────────────────────
local WHITE = disp.rgb(255, 255, 255)
disp.clear(WHITE)
if a then pcall(function() a:draw(99.99999, 50) end) end        -- x 100..110, y 50..53
pcall(function() b:draw(10.5, 70.5) end)                        -- x 11..12, y 71..72
pcall(function() b:draw(-0.4, 90) end)                          -- x 0..1,   y 90..91
disp.flush()

log("DONE", "1")

while true do
    pc.input.update()
    if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then return end
    pc.sys.sleep(20)
end
