-- appconfig bleed pair, second app. Launched right after cfg_a in the same
-- simulator: it must see its own (empty) store, not cfg_a's, and its save
-- must land in /data/com.test.cfg_b/config.json (the host checks the files).
local pc = picocalc
local T = pc.sys.loadlib("picotest")

T.case("b_does_not_see_a", function()
    T.eq(pc.config.get("k"), nil, "cfg_b read cfg_a's key")
end)

T.case("b_sets_and_saves", function()
    pc.config.set("b", "B")
    T.eq(pc.config.get("b"), "B")
    T.ok(pc.config.save(), "save failed")
end)

T.done()
