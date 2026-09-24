-- appconfig bleed pair, first app: store k=A under com.test.cfg_a.
local pc = picocalc
local T = pc.sys.loadlib("picotest")

T.case("a_sets_and_saves", function()
    pc.config.set("k", "A")
    T.eq(pc.config.get("k"), "A")
    T.ok(pc.config.save(), "save failed")
end)

T.done()
