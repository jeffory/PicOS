-- Heap probe: allocates about 2 MB of Lua heap and holds it until 'q', so a
-- test can check that get_heap_info actually reflects allocations.
local pc = picocalc
local hold = {}
for i = 1, 64 do
    hold[i] = string.rep(string.char(65 + i % 26), 32 * 1024)
end
pc.sys.log("HP:HOLD " .. #hold)
while true do
    pc.input.update()
    if pc.input.getChar() == "q" then break end
    pc.sys.sleep(10)
end
hold = nil
collectgarbage("collect")
pc.sys.log("HP:RELEASED")
