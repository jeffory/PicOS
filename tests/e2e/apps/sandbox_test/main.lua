-- Sandbox enforcement fixture (picotest kit). No requirements, so the app may
-- read /apps/sandbox_test and /system/lib, and read+write /data/com.test.sandbox.
--
-- The harness stages, before boot:
--   /system/config.json               {"sentinel":"keep"}
--   /data/com.other/config.json       another app's config
--   /data/com.other/secret.wav        another app's (valid) WAV
-- and checks on the host afterwards that nothing was written outside
-- /data/com.test.sandbox (test_sandbox.py).
--
-- Cases marked in test_sandbox.py as strict xfails are known review bugs
-- (the sandbox reads writable Lua globals; sound loads/saves skip the check;
-- appconfig trusts APP_ID). The kit restores the identity globals after
-- every case.

local pc = picocalc
local fs = pc.fs
local T = pc.sys.loadlib("picotest")

local OWN = "/data/com.test.sandbox"

local function try_write(path)
    local f = fs.open(path, "w")
    if f then
        fs.write(f, "pwned")
        fs.close(f)
        return true
    end
    return false
end

-- ── Baseline: the sandbox as designed ──────────────────────────────────────

T.case("read_own_app_dir", function()
    T.ok(fs.readFile(APP_DIR .. "/main.lua"), "cannot read own main.lua")
end)

T.case("write_own_data_dir", function()
    T.ok(fs.mkdir(OWN), "mkdir own data dir")
    T.ok(try_write(OWN .. "/x"), "cannot write own data dir")
    T.eq(fs.readFile(OWN .. "/x"), "pwned")
end)

T.case("read_system_lib", function()
    T.ok(fs.readFile("/system/lib/picotest.lua"), "cannot read /system/lib")
    T.eq(try_write("/system/lib/evil.lua"), false, "wrote into /system/lib")
end)

T.case("read_system_config_denied", function()
    T.eq(fs.readFile("/system/config.json"), nil, "read /system/config.json")
    T.eq(fs.exists("/system/config.json"), false, "exists() leaks /system")
end)

T.case("write_other_app_data_denied", function()
    T.eq(try_write("/data/com.other/baseline_x"), false, "wrote another app's data")
end)

T.case("read_other_app_data_denied", function()
    T.eq(fs.readFile("/data/com.other/config.json"), nil,
         "read another app's config")
end)

T.case("read_other_app_dir_denied", function()
    T.eq(fs.readFile("/apps/fs_test/main.lua"), nil, "read another app's code")
end)

T.case("relative_path_denied", function()
    T.eq(fs.readFile("apps/sandbox_test/main.lua"), nil, "relative read")
    T.eq(try_write("x"), false, "relative write")
end)

T.case("prefix_confusion_denied", function()
    T.eq(try_write("/data/com.test.sandboxEVIL/x"), false,
         "wrote /data/com.test.sandboxEVIL")
end)

T.case("dotdot_denied", function()
    T.eq(fs.readFile(OWN .. "/../../system/config.json"), nil, "read via ..")
    T.eq(try_write(OWN .. "/../com.other/x"), false, "write via ..")
    T.eq(fs.readFile(APP_DIR .. "/../fs_test/main.lua"), nil, "app dir via ..")
end)

T.case("dofile_loadfile_absent", function()
    T.eq(dofile, nil, "dofile")
    T.eq(loadfile, nil, "loadfile")
end)

-- ── Global mutation: the sandbox must not trust Lua-writable globals ──────

T.case("globals_root_flag_field", function()
    APP_REQUIREMENTS.root_filesystem = true
    T.eq(fs.readFile("/system/config.json"), nil,
         "APP_REQUIREMENTS.root_filesystem=true granted root access")
end)

T.case("globals_requirements_table", function()
    APP_REQUIREMENTS = { root_filesystem = true }
    T.eq(fs.readFile("/system/config.json"), nil,
         "a replaced APP_REQUIREMENTS table granted root access")
end)

T.case("globals_app_id", function()
    APP_ID = "com.other"
    -- Attempt both before checking, so the host-side check sees the write.
    local wrote = try_write("/data/com.other/globals_x")
    local read = fs.readFile("/data/com.other/config.json")
    T.eq(read, nil, "APP_ID=com.other read the other app's data")
    T.eq(wrote, false, "APP_ID=com.other wrote the other app's data")
end)

T.case("globals_app_dir", function()
    APP_DIR = "/apps/fs_test"
    T.eq(fs.readFile("/apps/fs_test/main.lua"), nil,
         "APP_DIR=/apps/fs_test read the other app's code")
end)

-- ── APIs that open files without the sandbox check ────────────────────────

T.case("sample_save_outside_sandbox", function()
    local s = T.ok(pc.sound.sample(0.01), "cannot create a blank sample")
    local ok = s:save("/system/pwn.wav")
    T.ok(not ok, "sample:save wrote /system/pwn.wav")
end)

T.case("sample_load_other_app", function()
    T.eq(pc.sound.sample("/data/com.other/secret.wav"), nil,
         "sound.sample loaded another app's file")
end)

T.case("sampleplayer_load_other_app", function()
    T.eq(pc.sound.sampleplayer("/data/com.other/secret.wav"), nil,
         "sound.sampleplayer loaded another app's file")
end)

-- ── appconfig: the store path must not come from APP_ID ────────────────────

T.case("appconfig_app_id_traversal", function()
    APP_ID = "../system"
    pc.config.set("pwned", "1")
    T.ok(pc.config.save(), "config.save() failed")
    -- The save landed in this app's own store (the host checks that
    -- /system/config.json is untouched).
    local own = fs.readFile(OWN .. "/config.json")
    T.ok(own and own:find('"pwned"', 1, true),
         "config.save() did not write /data/com.test.sandbox/config.json")
end)

T.done()
