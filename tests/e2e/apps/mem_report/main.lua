-- Logs the PSRAM heap and how many of 16 simultaneous file opens succeed
-- (FatFS allows 16, FF_FS_LOCK), then exits. test_native_resources.py runs
-- it before and after a leaky native app.
local pc = picocalc
local fs = pc.fs
collectgarbage("collect")
local handles, opened = {}, 0
for i = 1, 16 do
    local f = fs.open(APP_DIR .. "/main.lua", "r")
    if f then
        opened = opened + 1
        handles[#handles + 1] = f
    end
end
for _, f in ipairs(handles) do fs.close(f) end
handles = nil
collectgarbage("collect")
local m = pc.sys.getMemInfo()
pc.sys.log(string.format("MEM free=%d largest=%d files=%d",
    m.psram_free, m.psram_largest_block, opened))
