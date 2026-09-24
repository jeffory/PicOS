-- Harness fixture: waits for 'g', then logs ~18 MB of notifications — far
-- more than the simulator's 512 KB per-client buffer plus kernel socket
-- buffers — so a client that stops reading has notifications dropped. The
-- ring keeps the newest 4096 lines, so the final marker stays readable.
local pc = picocalc
local log = pc.sys.log
log("H:BIGFLOOD_READY")
while true do
  pc.input.update()
  if pc.input.getChar() == "g" then break end
  pc.sys.sleep(10)
end
local pad = string.rep("y", 900)
for i = 1, 20000 do log("H:BIGFLOOD " .. i .. " " .. pad) end
log("H:BIGFLOOD_DONE")
