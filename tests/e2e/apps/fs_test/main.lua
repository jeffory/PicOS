-- Filesystem test fixture app
-- Exercises fs operations and logs results for E2E verification.
-- Each test is a function that logs "PASS:<name>" or "FAIL:<name>:<reason>".

local pc = picocalc
local fs = pc.fs
local log = pc.sys.log
local input = pc.input

local function test_write_read()
    local path = fs.appPath("test_write.txt")
    local data = "Hello, PicOS filesystem!"
    local f = fs.open(path, "w")
    if not f then return "FAIL:write_read:open_w" end
    fs.write(f, data)
    fs.close(f)

    local f2 = fs.open(path, "r")
    if not f2 then return "FAIL:write_read:open_r" end
    local got = fs.read(f2, #data + 10)
    fs.close(f2)

    if got == data then
        return "PASS:write_read"
    else
        return "FAIL:write_read:mismatch:" .. tostring(got)
    end
end

local function test_readFile()
    local path = fs.appPath("test_readfile.txt")
    local data = "readFile test data 12345"
    local f = fs.open(path, "w")
    if not f then return "FAIL:readFile:open_w" end
    fs.write(f, data)
    fs.close(f)

    local got = fs.readFile(path)
    if got == data then
        return "PASS:readFile"
    else
        return "FAIL:readFile:mismatch:" .. tostring(got)
    end
end

local function test_exists()
    local path = fs.appPath("test_exists.txt")
    if fs.exists(path) then return "FAIL:exists:already_exists" end

    local f = fs.open(path, "w")
    if not f then return "FAIL:exists:open_w" end
    fs.write(f, "x")
    fs.close(f)

    if fs.exists(path) then
        return "PASS:exists"
    else
        return "FAIL:exists:not_found_after_write"
    end
end

local function test_size()
    local path = fs.appPath("test_size.txt")
    local data = "exactly 20 bytes!!!"  -- 19 chars + count
    -- Actually let's be precise
    data = string.rep("A", 100)
    local f = fs.open(path, "w")
    if not f then return "FAIL:size:open_w" end
    fs.write(f, data)
    fs.close(f)

    local sz = fs.size(path)
    if sz == 100 then
        return "PASS:size"
    else
        return "FAIL:size:expected_100_got_" .. tostring(sz)
    end
end

local function test_seek_tell()
    local path = fs.appPath("test_seek.txt")
    local f = fs.open(path, "w")
    if not f then return "FAIL:seek_tell:open_w" end
    fs.write(f, "ABCDEFGHIJ")
    fs.close(f)

    local f2 = fs.open(path, "r")
    if not f2 then return "FAIL:seek_tell:open_r" end
    fs.seek(f2, 5)
    local pos = fs.tell(f2)
    local got = fs.read(f2, 5)
    fs.close(f2)

    if pos == 5 and got == "FGHIJ" then
        return "PASS:seek_tell"
    else
        return "FAIL:seek_tell:pos=" .. tostring(pos) .. ",got=" .. tostring(got)
    end
end

local function test_mkdir()
    local dir = fs.appPath("test_subdir")
    local ok = fs.mkdir(dir)
    if not ok then return "FAIL:mkdir:failed" end

    -- Write a file inside the new directory
    local path = dir .. "/nested.txt"
    local f = fs.open(path, "w")
    if not f then return "FAIL:mkdir:open_nested" end
    fs.write(f, "nested content")
    fs.close(f)

    local got = fs.readFile(path)
    if got == "nested content" then
        return "PASS:mkdir"
    else
        return "FAIL:mkdir:nested_mismatch"
    end
end

local function test_delete()
    local path = fs.appPath("test_delete.txt")
    local f = fs.open(path, "w")
    if not f then return "FAIL:delete:open_w" end
    fs.write(f, "delete me")
    fs.close(f)

    if not fs.exists(path) then return "FAIL:delete:not_created" end
    fs.delete(path)
    if fs.exists(path) then
        return "FAIL:delete:still_exists"
    else
        return "PASS:delete"
    end
end

local function test_rename()
    local src = fs.appPath("test_rename_src.txt")
    local dst = fs.appPath("test_rename_dst.txt")
    local f = fs.open(src, "w")
    if not f then return "FAIL:rename:open_w" end
    fs.write(f, "rename data")
    fs.close(f)

    local ok = fs.rename(src, dst)
    if not ok then return "FAIL:rename:failed" end
    if fs.exists(src) then return "FAIL:rename:src_still_exists" end
    local got = fs.readFile(dst)
    if got == "rename data" then
        return "PASS:rename"
    else
        return "FAIL:rename:data_mismatch:" .. tostring(got)
    end
end

local function test_listDir()
    -- List the data dir (should have files from previous tests)
    local entries = fs.listDir(fs.appPath(""))
    if not entries then return "FAIL:listDir:nil" end
    if #entries == 0 then return "FAIL:listDir:empty" end

    -- Check that at least one known file is listed
    local found = false
    for _, e in ipairs(entries) do
        if e.name and not e.is_dir then
            found = true
            break
        end
    end
    if found then
        return "PASS:listDir"
    else
        return "FAIL:listDir:no_files_found"
    end
end

local function test_large_write()
    -- Write a larger file to exercise multi-block SD card path
    local path = fs.appPath("test_large.bin")
    local chunk = string.rep("X", 512)  -- 512 bytes per chunk
    local f = fs.open(path, "w")
    if not f then return "FAIL:large_write:open" end
    for i = 1, 64 do  -- 32KB total
        fs.write(f, chunk)
    end
    fs.close(f)

    local sz = fs.size(path)
    if sz == 32768 then
        return "PASS:large_write"
    else
        return "FAIL:large_write:size=" .. tostring(sz)
    end
end

local function test_large_read_back()
    -- Read back the large file and verify content
    local path = fs.appPath("test_large.bin")
    local f = fs.open(path, "r")
    if not f then return "FAIL:large_read:open" end
    local data = fs.read(f, 32768)
    fs.close(f)

    if not data then return "FAIL:large_read:nil_data" end
    if #data ~= 32768 then return "FAIL:large_read:len=" .. #data end

    -- Verify content
    local expected = string.rep("X", 512)
    for i = 0, 63 do
        local chunk = data:sub(i * 512 + 1, (i + 1) * 512)
        if chunk ~= expected then
            return "FAIL:large_read:corrupt_at_chunk_" .. i
        end
    end
    return "PASS:large_read"
end

local function test_diskInfo()
    local info = fs.diskInfo()
    if not info then return "FAIL:diskInfo:nil" end
    if not info.free or not info.total then return "FAIL:diskInfo:missing_fields" end
    if info.total > 0 and info.free >= 0 then
        return "PASS:diskInfo"
    else
        return "FAIL:diskInfo:invalid_values"
    end
end

-- Run all tests
local tests = {
    test_write_read,
    test_readFile,
    test_exists,
    test_size,
    test_seek_tell,
    test_mkdir,
    test_delete,
    test_rename,
    test_listDir,
    test_large_write,
    test_large_read_back,
    test_diskInfo,
}

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Running FS tests...", pc.display.WHITE)
pc.display.flush()

for _, test in ipairs(tests) do
    local ok, result = pcall(test)
    if ok then
        log(result)
    else
        log("FAIL:exception:" .. tostring(result))
    end
end

log("FS_TESTS_DONE")

-- Wait briefly for log buffer to flush, then exit
pc.sys.sleep(100)
