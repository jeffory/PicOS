-- Object lifetimes under garbage collection (audit §3.3/§3.4), picotest kit.
--
-- Each anchor case builds a parent -> child relationship, drops every Lua
-- reference to the child, collects twice, churns ~1 MB so freed memory gets
-- reused, and then checks through a weak table that the child is still
-- alive. A child the parent points at from C must be kept alive by the
-- parent (a uservalue or registry anchor); if it is collected, the parent
-- holds a dangling pointer. Only when the child survived does the case go on
-- to use the parent, so today's failures stay deterministic instead of
-- turning into use-after-free in the release simulator. (The sanitizer build,
-- Task 22, is what catches the use-after-free itself.)
--
-- If /apps/gc_test/stress.flag exists the collector runs in a very
-- aggressive incremental mode for the whole run (the harness runs both).
--
-- use_first.flag (sanitizer runs only): skip the survival check until after
-- the parent has been used, so a collected child is a real use-after-free
-- that ASan reports at the bridge call. only.flag names the one case to run
-- (a use-after-free aborts the sanitizer build, so each case gets its own
-- run). See test_lifetimes.py::test_gc_use_after_collect.

local pc = picocalc
local gfx = pc.graphics
local sound = pc.sound
local fs = pc.fs
local T = pc.sys.loadlib("picotest")

local STRESS = fs.exists(APP_DIR .. "/stress.flag")
local USE_FIRST = fs.exists(APP_DIR .. "/use_first.flag")
local ONLY = fs.exists(APP_DIR .. "/only.flag") and fs.readFile(APP_DIR .. "/only.flag")
if ONLY then
    local case = T.case
    T.case = function(name, fn)
        if name == ONLY then return case(name, fn) end
    end
end
if STRESS then
    collectgarbage("incremental", 0, 1000)
end
pc.sys.log("GC:MODE " .. (STRESS and "stress" or "normal"))

local function churn()
    collectgarbage("collect")
    collectgarbage("collect")
    local junk = {}
    for i = 1, 1000 do junk[i] = string.rep("j", 1000) .. i end
    junk = nil
    collectgarbage("collect")
end

-- Holds `make()`'s child only weakly; returns the parent, the weak table and
-- the key. make() must return parent, child.
local function weakly(make)
    local weak = setmetatable({}, { __mode = "v" })
    local parent, child = make()
    weak[1] = child
    child = nil
    return parent, weak
end

local function alive(weak, what)
    churn()
    if USE_FIRST then return end  -- settle() checks, after the parent is used
    T.ok(weak[1] ~= nil, what .. " was collected while still referenced from C")
end

-- End of an anchor case: in use_first mode, the survival check alive()
-- skipped (reached only if using the parent did not trip the sanitizer).
local function settle(weak, what)
    if USE_FIRST then
        T.ok(weak[1] ~= nil, what .. " was collected while still referenced from C")
    end
end

-- Run body; always run cleanup (so a failed case can't leave dangling
-- entries in the sprite list for later cases).
local function guarded(body, cleanup)
    local ok, err = pcall(body)
    cleanup()
    if not ok then error(err, 0) end
end

T.case("sprite_new_keeps_image", function()
    local s, weak = weakly(function()
        local img = gfx.image.new(16, 16)
        return gfx.sprite.new(img), img
    end)
    alive(weak, "the image passed to sprite.new")
    local w, h = s:getSize()
    T.eq(w, 16, "width")
    T.eq(h, 16, "height")
    s:draw()  -- reads the image's pixels (getSize is cached on the sprite)
    settle(weak, "the image passed to sprite.new")
end)

T.case("sprite_setImage_keeps_image", function()
    local s, weak = weakly(function()
        local img = gfx.image.new(16, 16)
        local sp = gfx.sprite.new()
        sp:setImage(img)
        return sp, img
    end)
    alive(weak, "the image passed to sprite:setImage")
    local w = s:getSize()
    T.eq(w, 16, "width")
    s:draw()
    settle(weak, "the image passed to sprite:setImage")
end)

local KEEP_IMG = gfx.image.new(8, 8)

T.case("added_sprite_survives_gc", function()
    guarded(function()
        local weak = setmetatable({}, { __mode = "v" })
        do
            local s = gfx.sprite.new(KEEP_IMG)
            s:moveTo(40, 40)
            s:add()
            weak[1] = s
        end
        alive(weak, "a sprite in the display list")
        T.eq(gfx.sprite.spriteCount(), 1, "sprite count")
        gfx.sprite.update()
        pc.display.flush()
        settle(weak, "a sprite in the display list")
    end, function() gfx.sprite.removeAll() end)
end)

T.case("spritesheet_keeps_image", function()
    local sheet, weak = weakly(function()
        local img = gfx.image.new(32, 32)
        return gfx.spritesheet.new(img), img
    end)
    alive(weak, "the image passed to spritesheet.new")
    sheet:addFrame(0, 0, 16, 16)
    sheet:drawFrame(0, 0, 0)  -- frames are 0-based
    settle(weak, "the image passed to spritesheet.new")
end)

T.case("tilemap_keeps_tileset", function()
    local map, weak = weakly(function()
        local img = gfx.image.new(32, 32)
        return gfx.tilemap.new(img, 8, 8), img
    end)
    alive(weak, "the tileset passed to tilemap.new")
    map:setSize(2, 2)
    map:setTileAtPosition(0, 0, 1)  -- 0-based cell, 1-based tile
    map:draw(0, 0)
    settle(weak, "the tileset passed to tilemap.new")
end)

T.case("animation_loop_keeps_frames", function()
    local loop, weak = weakly(function()
        local img = gfx.image.new(8, 8)
        return gfx.animation.loop.new(100, { img }), img
    end)
    alive(weak, "a frame passed to animation.loop.new")
    loop:draw(0, 0)
    settle(weak, "a frame passed to animation.loop.new")
end)

T.case("sampleplayer_setSample_keeps_sample", function()
    local p, weak = weakly(function()
        local sample = T.ok(sound.sample(0.05), "blank sample")
        local player = T.ok(sound.sampleplayer(), "player")
        player:setSample(sample)
        return player, sample
    end)
    alive(weak, "the sample passed to sampleplayer:setSample")
    p:play()
    pc.sys.sleep(20)
    p:stop()
    settle(weak, "the sample passed to sampleplayer:setSample")
end)

T.case("sampleplayer_new_keeps_sample", function()
    local p, weak = weakly(function()
        local sample = T.ok(sound.sample(0.05), "blank sample")
        return T.ok(sound.sampleplayer(sample), "player"), sample
    end)
    alive(weak, "the sample passed to sound.sampleplayer")
    p:play()
    pc.sys.sleep(20)
    p:stop()
    settle(weak, "the sample passed to sound.sampleplayer")
end)

T.case("performOnAllSprites_visits_real_sprites", function()
    local kept = {}
    guarded(function()
        for i = 1, 3 do
            local s = gfx.sprite.new(KEEP_IMG)
            s:moveTo(10 * i, 10)
            s:add()
            kept[i] = s
        end
        local visited = 0
        gfx.sprite.performOnAllSprites(function(s)
            visited = visited + 1
            s:moveBy(1, 0)
        end)
        churn()
        T.eq(visited, 3, "callbacks")
        for i = 1, 3 do
            local x = kept[i]:getPosition()
            T.eq(x, 10 * i + 1, "sprite " .. i .. " x after moveBy")
        end
        gfx.sprite.update()
    end, function() gfx.sprite.removeAll() end)
end)

T.case("fs_close_rejects_non_handles", function()
    -- Must not crash: nil, strings, numbers and tables are not handles.
    for _, bad in ipairs({ "x", 123, {} }) do
        pcall(fs.close, bad)
        pcall(fs.read, bad, 10)
        pcall(fs.write, bad, "x")
    end
    pcall(fs.close, nil)
    local f = T.ok(fs.open(fs.appPath("after_bad_close.txt"), "w"), "open after")
    fs.write(f, "ok")
    fs.close(f)
end)

T.done()
