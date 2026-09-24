-- Harness fixture: a log line every 20 ms for 8 s (sys.log must never
-- stall on a slow socket client).
local sys = picocalc.sys
for i = 1, 400 do
  sys.log("H:TICK " .. i)
  sys.sleep(20)
end
sys.log("H:TICK_DONE")
