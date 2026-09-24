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

local pc = picocalc
local gfx = pc.graphics
local sound = pc.sound
local fs = pc.fs
local T = pc.sys.loadlib("picotest")

local STRESS = fs.exists(APP_DIR .. "/stress.flag")
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
    T.ok(weak[1] ~= nil, what .. " was collected while still referenced from C")
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
    end, function() gfx.sprite.removeAll() end)
end)

T.case("spritesheet_keeps_image", function()
    local _, weak = weakly(function()
        local img = gfx.image.new(32, 32)
        return gfx.spritesheet.new(img), img
    end)
    alive(weak, "the image passed to spritesheet.new")
end)

T.case("tilemap_keeps_tileset", function()
    local _, weak = weakly(function()
        local img = gfx.image.new(32, 32)
        return gfx.tilemap.new(img, 8, 8), img
    end)
    alive(weak, "the tileset passed to tilemap.new")
end)

T.case("animation_loop_keeps_frames", function()
    local _, weak = weakly(function()
        local img = gfx.image.new(8, 8)
        return gfx.animation.loop.new(100, { img }), img
    end)
    alive(weak, "a frame passed to animation.loop.new")
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
