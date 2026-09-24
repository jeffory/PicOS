-- game.save name handling (audit §3.2), picotest kit. No requirements.
--
-- Saves live in /data/<app_id>/saves/<name>.json; names are limited to
-- [A-Za-z0-9._-], 1-128 bytes, no "..", no leading ".". The harness stages
-- /system/config.json {"sentinel":"keep"}, a corrupt
-- /data/com.test.save/saves/bad.json and a legacy /saves/legacy.json, and
-- checks on the host that no hostile name touched anything outside the save
-- area and that the saves landed in the app's data dir.
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

T.case("list_returns_saved_names", function()
    T.ok(save.set("list_a", { v = 1 }), "set list_a")
    T.ok(save.set("list_b", { v = 2 }), "set list_b")
    local names = {}
    for _, n in ipairs(save.list()) do names[n] = true end
    T.ok(names.list_a, "list() lacks list_a")
    T.ok(names.list_b, "list() lacks list_b")
    T.ok(names.ok_slot, "list() lacks ok_slot")
    T.ok(not names["list_a.json"], "list() kept the .json suffix")
    T.ok(save.delete("list_b"), "delete list_b")
    names = {}
    for _, n in ipairs(save.list()) do names[n] = true end
    T.ok(not names.list_b, "list() still has deleted list_b")
end)

T.case("legacy_save_migrates", function()
    -- Staged at the pre-Task-6 location /saves/legacy.json.
    T.ok(save.exists("legacy"), "legacy save not found")
    local b = T.ok(save.get("legacy"), "legacy get")
    T.eq(b.high_score, 695, "high_score")
end)

T.case("bad_names_rejected", function()
    local bad = { "", ".", "..", ".hidden", "a\\b", "a\0b", "a b", "a:b",
                  "x/../y", string.rep("n", 129) }
    for _, n in ipairs(bad) do
        local ok, err = save.set(n, { v = 1 })
        T.ok(not ok, "set accepted " .. string.format("%q", n))
        T.eq(err, "invalid save name", "set error for " .. string.format("%q", n))
        T.eq(save.get(n), nil, "get " .. string.format("%q", n))
        T.eq(save.exists(n), false, "exists " .. string.format("%q", n))
        T.eq(save.delete(n), false, "delete " .. string.format("%q", n))
    end
    T.ok(save.set(string.rep("n", 128), { v = 1 }), "128-byte name refused")
    T.ok(save.set("A-z_0.9", { v = 1 }), "charset name refused")
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
