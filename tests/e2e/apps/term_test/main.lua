-- Terminal buffer test fixture
-- Creates a terminal, writes known text, then waits for ESC to exit.

local pc = picocalc
local input = pc.input

local term = pc.terminal.new(53, 26)
term:clear()
term:write("Hello, World!\n")
term:write("Line 2 here")
term:render()
pc.display.flush()
pc.sys.log("TERM_READY")

-- Wait for ESC to exit
while true do
    input.update()
    local held = input.getButtons()
    if (held & input.BTN_ESC) ~= 0 then
        return
    end
    pc.sys.sleep(16)
end
