-- Control for sandbox_test: with "root-filesystem" the same operations the
-- sandbox denies must succeed, so a denial there is the sandbox's doing.

local pc = picocalc
local fs = pc.fs
local T = pc.sys.loadlib("picotest")

T.case("root_reads_system_config", function()
    T.ok(fs.readFile("/system/config.json"), "cannot read /system/config.json")
end)

T.case("root_reads_other_app_dir", function()
    T.ok(fs.readFile("/apps/fs_test/main.lua"), "cannot read /apps/fs_test")
end)

T.case("root_writes_other_app_data", function()
    local path = "/data/com.other/root_probe"
    local f = T.ok(fs.open(path, "w"), "cannot open " .. path)
    fs.write(f, "root")
    fs.close(f)
    T.eq(fs.readFile(path), "root")
    fs.delete(path)
    T.eq(fs.exists(path), false, "cleanup")
end)

T.done()
