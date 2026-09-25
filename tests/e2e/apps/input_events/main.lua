-- Input event queue fixture (tests/e2e/test_input_events.py).
-- Each phase logs IE:READY <n>, then sleeps (sys.sleep does not poll the
-- keyboard) while the test injects keys, so everything lands in ONE
-- input.update(). The app then reports what it saw.

local pc = picocalc
local input = pc.input
local log = pc.sys.log

local function fmt(e)
    return string.format("IE:EV %s %d %s %d %s %s", e.type, e.key,
        e.char and string.format("%d", string.byte(e.char)) or "-",
        e.mods, e.button and tostring(e.button) or "-",
        e["repeat"] and "R" or "-")  -- `repeat` is a Lua keyword
end

local function drain()
    while true do
        local e = input.pollEvent()
        if not e then break end
        log(fmt(e))
    end
end

local function phase(n)
    log("IE:READY " .. n)
    pc.sys.sleep(700)
end

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Input events", pc.display.WHITE)
pc.display.flush()

log("IE:HAS " .. type(input.pollEvent) .. " " .. type(input.isKeyDown))
log("IE:EMPTY " .. tostring(input.pollEvent and input.pollEvent()))

-- 1. Ordering: chars and a button injected in one frame arrive in order.
phase(1)
input.update()
drain()
log("IE:NIL " .. tostring(input.pollEvent()))
log("IE:DONE 1")

-- 2. Two chars in one frame: getChar hands both out, one per update().
phase(2)
local got = {}
for _ = 1, 10 do
    input.update()
    local c = input.getChar()
    if c then got[#got + 1] = c end
    pc.sys.sleep(16)
end
log("IE:GETCHAR " .. table.concat(got))
drain()
log("IE:DONE 2")

-- 3. isKeyDown follows a held key; mods ride on events.
phase(3)
input.update()
drain()
log(string.format("IE:DOWN ctrl=%s k=%s K=%s held=%s", tostring(input.isKeyDown(0xA5)),
    tostring(input.isKeyDown("k")), tostring(input.isKeyDown("K")),
    tostring((input.getButtons() & input.BTN_CTRL) ~= 0)))
log("IE:HELD 3")
local up = false
for _ = 1, 150 do
    input.update()
    if not input.isKeyDown(0xA5) then up = true break end
    pc.sys.sleep(16)
end
log("IE:RELEASED " .. tostring(up))
drain()
local ok1 = pcall(input.isKeyDown, "ab")
local ok2 = pcall(input.isKeyDown, 300)
log("IE:BADARGS " .. tostring(ok1) .. " " .. tostring(ok2))
log("IE:DONE 3")

-- 4. clearState empties the queue.
phase(4)
input.update()
input.clearState()
log("IE:CLEARED " .. tostring(input.pollEvent()))
log("IE:DONE 4")
