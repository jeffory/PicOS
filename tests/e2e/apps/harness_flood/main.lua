-- Harness fixture: log more lines than the simulator's log ring holds.
local log = picocalc.sys.log
local pad = string.rep("x", 200)
for i = 1, 5000 do log("H:FLOOD " .. i .. " " .. pad) end
log("H:FLOOD_DONE")
