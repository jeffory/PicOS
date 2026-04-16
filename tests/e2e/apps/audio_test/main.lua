-- Audio test fixture
-- Exercises picocalc.audio.* APIs
-- Each test logs "PASS:<name>" or "FAIL:<name>:<reason>"

local pc = picocalc
local audio = pc.audio
local log = pc.sys.log

local function test_setVolume()
    local ok, err = pcall(function()
        audio.setVolume(50)
    end)
    if ok then
        return "PASS:setVolume"
    else
        return "FAIL:setVolume:" .. tostring(err)
    end
end

local function test_playTone()
    local ok, err = pcall(function()
        audio.playTone(440, 500)  -- 440Hz for 500ms
    end)
    if ok then
        return "PASS:playTone"
    else
        return "FAIL:playTone:" .. tostring(err)
    end
end

local function test_stopTone()
    -- Play a tone first, then stop it
    pcall(function() audio.playTone(880, 2000) end)
    pc.sys.sleep(50)
    local ok, err = pcall(function()
        audio.stopTone()
    end)
    if ok then
        return "PASS:stopTone"
    else
        return "FAIL:stopTone:" .. tostring(err)
    end
end

local function test_volume_range()
    -- Test volume boundaries
    local ok1 = pcall(function() audio.setVolume(0) end)
    local ok2 = pcall(function() audio.setVolume(100) end)
    local ok3 = pcall(function() audio.setVolume(50) end)
    if ok1 and ok2 and ok3 then
        return "PASS:volume_range"
    else
        return "FAIL:volume_range:boundary_error"
    end
end

-- Run all tests
local tests = {
    test_setVolume,
    test_playTone,
    test_stopTone,
    test_volume_range,
}

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Running audio tests...", pc.display.WHITE)
pc.display.flush()

for _, test in ipairs(tests) do
    local ok, result = pcall(test)
    if ok then
        log(result)
    else
        log("FAIL:exception:" .. tostring(result))
    end
end

log("AUDIO_TESTS_DONE")
pc.sys.sleep(100)
