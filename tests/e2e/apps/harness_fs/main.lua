-- Harness fixture: game.save builds "/saves/<name>.json" without checking the
-- name, so "../../escape_probe" climbs one level above the SD root. The
-- simulator must refuse it instead of writing to the host disk.
local ok, err = picocalc.game.save.set("../../escape_probe", {a = 1})
picocalc.sys.log("H:ESCAPE " .. tostring(ok) .. " " .. tostring(err))
-- A ".." that stays inside the root still works.
local ok2 = picocalc.game.save.set("../saves/inside_probe", {b = 2})
picocalc.sys.log("H:INSIDE " .. tostring(ok2))
