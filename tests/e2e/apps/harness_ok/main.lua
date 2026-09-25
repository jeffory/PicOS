-- Harness fixture: a few markers, then return normally.
local log = picocalc.sys.log
log("H:START")
for i = 1, 5 do log("H:LINE " .. i) end
-- Boot parity: /system/config.json is loaded at boot (the test writes it).
log("H:CFG " .. tostring(picocalc.sysconfig.get("harness_key")))
-- Host-file loaders are removed from the base library.
log("H:DOFILE " .. tostring(dofile) .. " " .. tostring(loadfile))
log("H:DONE")
