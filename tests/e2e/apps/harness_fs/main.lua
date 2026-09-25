-- Harness fixture: game.save names are one path component, so neither a
-- name that would climb out of the SD root nor one that stays inside it
-- reaches the filesystem. The simulator's own SD-root containment is pinned
-- by tests/unit/test_sim_sdcard.c.
local ok, err = picocalc.game.save.set("../../escape_probe", {a = 1})
picocalc.sys.log("H:ESCAPE " .. tostring(ok) .. " " .. tostring(err))
local ok2, err2 = picocalc.game.save.set("../saves/inside_probe", {b = 2})
picocalc.sys.log("H:INSIDE " .. tostring(ok2) .. " " .. tostring(err2))
