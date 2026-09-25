-- Static screen fixture: every frame is identical, so a display diff or a
-- pixel probe only changes when something else (the system menu) draws.
-- White field with a black band; 'q' or Esc returns to the launcher.
local pc = picocalc
local d = pc.display
local input = pc.input

local function draw()
    d.clear(d.WHITE)
    d.fillRect(0, 150, 320, 20, d.BLACK)
    d.flush()
end

draw()
pc.sys.log("SS:READY")
while true do
    input.update()
    local ch = input.getChar()
    if ch == "q" or (input.getButtonsPressed() & input.BTN_ESC) ~= 0 then
        pc.sys.log("SS:EXIT")
        return
    end
    draw()
    pc.sys.sleep(10)
end
