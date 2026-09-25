-- Launched after save_test in the same simulator: another app's save slot
-- must not be readable (saves are per-app once Task 6 lands).
local pc = picocalc
local T = pc.sys.loadlib("picotest")

T.case("isolation_peer_cannot_read", function()
    T.eq(pc.game.save.get("slot1"), nil, "read save_test's slot1")
end)

T.done()
