-- game.save name handling (audit §3.2), picotest kit. No requirements.
--
-- The harness stages /system/config.json {"sentinel":"keep"} and a corrupt
-- /saves/bad.json, and checks on the host that no hostile name touched
-- anything outside the save area. Today saves live in the shared /saves;
-- Task 6 moves them to /data/<app_id>/saves, which these cases allow for.
local pc = picocalc
local save = pc.game.save
local T = pc.sys.loadlib("picotest")

T.case("roundtrip", function()
    T.ok(save.set("ok_slot", { v = 1, s = "text" }), "set")
    local back = T.ok(save.get("ok_slot"), "get")
    T.eq(back.v, 1)
    T.eq(back.s, "text")
    T.ok(save.exists("ok_slot"), "exists")
end)

T.case("corrupt_file_reads_nil", function()
    T.eq(save.get("bad"), nil, "a corrupt save must read as no save")
end)

T.case("subdir_name_rejected", function()
    T.ok(not save.set("a/b", { v = 1 }), "set('a/b') succeeded")
end)

T.case("escape_sd_root_rejected", function()
    T.ok(not save.set("../../escape", { v = 1 }), "set('../../escape') succeeded")
end)

T.case("traversal_set_rejected", function()
    T.ok(not save.set("../system/pwn", { a = 1 }), "set('../system/pwn') succeeded")
end)

T.case("traversal_exists_rejected", function()
    T.eq(save.exists("../system/config"), false, "exists() saw /system/config.json")
end)

T.case("traversal_get_rejected", function()
    T.eq(save.get("../system/config"), nil, "get() read /system/config.json")
end)

T.case("long_names_do_not_collide", function()
    -- Two names that differ only after the 248th character.
    local a = string.rep("n", 260) .. "1"
    local b = string.rep("n", 260) .. "2"
    local ok_a = save.set(a, { v = 1 })
    local ok_b = save.set(b, { v = 2 })
    if ok_a then
        local back = save.get(a)
        T.eq(back and back.v, 1, "the second long name overwrote the first")
    else
        T.ok(not ok_b, "one long name accepted, the other rejected")
    end
end)

T.case("isolation_write", function()
    T.ok(save.set("slot1", { owner = "save_test" }), "set slot1")
end)

-- Destructive; runs last. Today this deletes /system/config.json.
T.case("traversal_delete_rejected", function()
    T.ok(not save.delete("../system/config"), "delete('../system/config') succeeded")
end)

T.done()
