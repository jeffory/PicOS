-- Graphics object lifetimes (review: Graphics Critical rows 1-2, High
-- "addWallSprites ... lua_checkstack", Medium "sprite size larger than the
-- image"; Task 7 raw-pointer acceptance). picotest kit.
--
-- The harness runs every case ALONE in a fresh simulator (only.flag names
-- it), once with the default collector and once with a very aggressive
-- incremental one (stress.flag). One case per run, because before the fix a
-- case is a use-after-free or a heap overflow: under the ASan simulator it
-- aborts at the bridge call, and in the release simulator it may corrupt
-- the heap, and neither may cost the other cases their result.
--
-- So cases use the parent object straight after the collection (that is the
-- use-after-free ASan reports) and only then check, through a weak table,
-- that the child survived: the release-visible half of the assertion.
-- See tests/e2e/test_lifetimes_graphics.py.

local pc = picocalc
local gfx = pc.graphics
local fs = pc.fs
local T = pc.sys.loadlib("picotest")

local STRESS = fs.exists(APP_DIR .. "/stress.flag")
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
pc.sys.log("GL:MODE " .. (STRESS and "stress" or "normal"))

-- Collect ten times and churn ~1 MB so freed blocks get reused.
local function churn()
    for _ = 1, 10 do collectgarbage("collect") end
    local junk = {}
    for i = 1, 1000 do junk[i] = string.rep("j", 1000) .. i end
    junk = nil
    collectgarbage("collect")
end

local function weak_ref(v)
    return setmetatable({ v }, { __mode = "v" })
end

local function survived(weak, what)
    T.ok(weak[1] ~= nil, what .. " was collected while still referenced from C")
end

-- Always empty the display list, even when the case fails.
local function guarded(body)
    local ok, err = pcall(body)
    gfx.sprite.removeAll()
    if not ok then error(err, 0) end
end

-- A w x h 16bpp top-down BMP of one colour.
local function bmp(w, h, colour)
    local pixels = string.rep(string.pack("<I2", colour), w * h)
    local header = string.pack("<c2I4I2I2I4", "BM", 54 + #pixels, 0, 0, 54)
    local info = string.pack("<I4i4i4I2I2I4I4i4i4I4I4",
        40, w, -h, 1, 16, 0, #pixels, 2835, 2835, 0, 0)
    return header .. info .. pixels
end

-- ── (a) the display list keeps an added sprite alive ────────────────────────

T.case("added_local_sprite_survives", function()
    guarded(function()
        local weak
        do
            local img = gfx.image.new(8, 8)
            local s = gfx.sprite.new(img)
            s:moveTo(40, 40)
            s:add()
            weak = weak_ref(s)
        end
        churn()
        T.eq(gfx.sprite.spriteCount(), 1, "sprite count")
        gfx.sprite.update()          -- reads the sprite and its image
        pc.display.flush()
        survived(weak, "a sprite in the display list")
        -- remove() releases the display list's reference.
        weak[1]:remove()
        churn()
        T.eq(gfx.sprite.spriteCount(), 0, "sprite count after remove")
        T.eq(weak[1], nil, "a removed sprite stayed referenced")
    end)
end)

T.case("add_twice_remove_once", function()
    guarded(function()
        local s = gfx.sprite.new(gfx.image.new(4, 4))
        s:add()
        s:add()
        T.eq(gfx.sprite.spriteCount(), 1, "add is idempotent")
        s:remove()
        T.eq(gfx.sprite.spriteCount(), 0, "one remove empties the list")
    end)
end)

T.case("empty_collision_sprite_survives", function()
    guarded(function()
        local weak = weak_ref(gfx.sprite.addEmptyCollisionSprite(10, 10, 20, 20))
        churn()
        local hits = gfx.sprite.querySpritesAtPoint(15, 15)
        T.eq(#hits, 1, "hits")
        hits[1]:moveBy(1, 0)
        T.eq(hits[1]:getPosition(), 11, "x after moveBy")
        survived(weak, "an empty collision sprite")
    end)
end)

-- ── (b) sprite enumeration hands out the real sprites ───────────────────────

T.case("performOnAllSprites_moveBy_then_gc", function()
    guarded(function()
        local img = gfx.image.new(4, 4)
        for i = 1, 3 do          -- held by the display list only
            local s = gfx.sprite.new(img)
            s:moveTo(10 * i, 10)
            s:add()
        end
        local visited = 0
        gfx.sprite.performOnAllSprites(function(s)
            visited = visited + 1
            s:moveBy(1, 0)
        end)
        churn()
        T.eq(visited, 3, "callbacks")
        local all = gfx.sprite.getAllSprites()
        T.eq(#all, 3, "getAllSprites")
        for i = 1, 3 do
            T.eq(all[i]:getPosition(), 10 * i + 1, "sprite " .. i .. " x")
        end
        gfx.sprite.update()
        pc.display.flush()
    end)
end)

T.case("performOnAllSprites_callback_may_remove", function()
    guarded(function()
        local img = gfx.image.new(4, 4)
        for _ = 1, 4 do gfx.sprite.new(img):add() end
        local visited = 0
        gfx.sprite.performOnAllSprites(function(s)
            visited = visited + 1
            s:remove()
            collectgarbage("collect")
        end)
        T.eq(visited, 4, "callbacks")
        T.eq(gfx.sprite.spriteCount(), 0, "count")
    end)
end)

T.case("performOnAllSprites_propagates_errors", function()
    guarded(function()
        gfx.sprite.new(gfx.image.new(4, 4)):add()
        T.raises(function()
            gfx.sprite.performOnAllSprites(function() error("boom") end)
        end, "boom")
    end)
end)

T.case("enumeration_returns_identical_sprites", function()
    guarded(function()
        local a = gfx.sprite.new(gfx.image.new(8, 8))
        local b = gfx.sprite.new(gfx.image.new(8, 8))
        for _, s in ipairs({ a, b }) do
            s:moveTo(50, 50)
            s:setCollisionsEnabled(true)
            s:setCollideRect(0, 0, 8, 8)
            s:add()
        end
        T.ok(gfx.sprite.getAllSprites()[1] == a, "getAllSprites()[1] is a")
        T.ok(a:overlappingSprites()[1] == b, "overlappingSprites")
        local pair = a:allOverlappingSprites()[1]   -- registered as a method
        T.ok(pair[1] == a and pair[2] == b, "allOverlappingSprites")
        T.ok(gfx.sprite.querySpritesAtPoint(52, 52)[1] == a, "querySpritesAtPoint")
        T.ok(gfx.sprite.querySpritesInRect(0, 0, 100, 100)[2] == b, "querySpritesInRect")
        T.ok(gfx.sprite.querySpritesAlongLine(0, 54, 100, 54)[1] == a, "querySpritesAlongLine")
        T.ok(gfx.sprite.querySpriteInfoAlongLine(0, 54, 100, 54)[1].sprite == a,
             "querySpriteInfoAlongLine")
        b:moveTo(70, 50)
        local _, _, cols = a:moveWithCollisions(66, 50)
        T.ok(cols[1] and cols[1].sprite == a and cols[1].other == b,
             "moveWithCollisions sprite/other")
        -- A method call on each result must work (a 4-byte proxy overflowed).
        for _, s in ipairs(gfx.sprite.querySpritesInRect(0, 0, 320, 320)) do
            s:setTag(7)
        end
        T.eq(a:getTag(), 7, "tag via query result")
    end)
end)

-- ── (c) addWallSprites: 200 walls ───────────────────────────────────────────

T.case("addWallSprites_200", function()
    guarded(function()
        local map = gfx.tilemap.new(gfx.image.new(16, 16), 8, 8)
        map:setSize(20, 10)
        for y = 0, 9 do
            for x = 0, 19 do map:setTileAtPosition(x, y, 1) end
        end
        T.eq(gfx.sprite.addWallSprites(map, { 1 }), 200, "walls added")
        churn()
        T.eq(gfx.sprite.spriteCount(), 200, "sprite count")
        local hit = gfx.sprite.querySpritesAtPoint(8 * 19 + 1, 8 * 9 + 1)
        T.eq(#hit, 1, "wall at the last tile")
        T.eq(hit[1]:getPosition(), 8 * 19, "wall x")
        gfx.sprite.update()
    end)
end)

-- ── C -> C pointers: the parent anchors its child ───────────────────────────

T.case("getImage_returns_the_image", function()
    local img = gfx.image.new(8, 8)
    local s = gfx.sprite.new(img)
    T.ok(s:getImage() == img, "sprite:getImage()")
    T.ok(s.image == img, "sprite.image")
    local sheet = gfx.spritesheet.new(img)
    T.ok(sheet:getImage() == img, "spritesheet:getImage()")
    local loop = gfx.animation.loop.new(100, { img })
    T.ok(loop:image() == img, "loop:image()")
    s:setImage(nil)
    T.eq(s:getImage(), nil, "after setImage(nil)")
end)

T.case("sprite_copy_keeps_image", function()
    local copy, weak
    do
        local img = gfx.image.new(8, 8)
        copy = gfx.sprite.new(img):copy()
        weak = weak_ref(img)
    end
    churn()
    copy:draw(0, 0)
    survived(weak, "the image of a copied sprite")
end)

T.case("spriteWithText_keeps_image", function()
    local s = T.ok(gfx.sprite.spriteWithText("hello", 64, 16), "sprite")
    churn()
    s:draw(0, 0)
    local img = s:getImage()
    T.eq(select(1, img:getSize()), 64, "image width")
end)

T.case("setTilemap_keeps_tilemap", function()
    guarded(function()
        local s, weak_map, weak_img
        do
            local img = gfx.image.new(16, 16)
            local map = gfx.tilemap.new(img, 8, 8)
            map:setSize(4, 4)
            map:setTileAtPosition(1, 1, 2)
            s = gfx.sprite.new()
            s:setTilemap(map)
            s:add()
            weak_map, weak_img = weak_ref(map), weak_ref(img)
        end
        churn()
        gfx.sprite.update()          -- draws the tilemap through the sprite
        survived(weak_map, "the tilemap of a sprite")
        survived(weak_img, "the tileset of a sprite's tilemap")
        s:setTilemap(nil)
        s:remove()
        churn()
        T.eq(weak_map[1], nil, "setTilemap(nil) released the tilemap")
    end)
end)

T.case("setStencilImage_keeps_image", function()
    local s = gfx.sprite.new(gfx.image.new(8, 8))
    local weak
    do
        local st = gfx.image.new(8, 8)
        s:setStencilImage(st)
        weak = weak_ref(st)
    end
    churn()
    survived(weak, "a sprite's stencil image")
    s:clearStencil()
    churn()
    T.eq(weak[1], nil, "clearStencil released the stencil")
end)

T.case("setImage_keeps_then_releases_image", function()
    local s = gfx.sprite.new()
    local weak
    do
        local a = gfx.image.new(8, 8)
        s:setImage(a)
        weak = weak_ref(a)
    end
    churn()
    s:draw(0, 0)
    survived(weak, "the image passed to setImage")
    s:setImage(gfx.image.new(4, 4))
    churn()
    T.eq(weak[1], nil, "the replaced image stayed referenced")
    T.eq(s:getSize(), 4, "new width")
    s:draw(0, 0)
end)

T.case("loop_setImageTable_keeps_frames", function()
    local loop = gfx.animation.loop.new(100, {})
    local weak
    do
        local img = gfx.image.new(8, 8)
        local frames = { img }
        loop:setImageTable(frames)
        frames[1] = nil              -- the caller's table no longer holds it
        weak = weak_ref(img)
    end
    churn()
    loop:draw(0, 0)
    survived(weak, "a frame passed to loop:setImageTable")
end)

-- ── Blinkers: the updateAll list must not outlive a collected blinker ───────

T.case("collected_blinker_leaves_updateAll", function()
    do
        local b = gfx.animation.blinker.new(10, 10, true)
        b:start()
    end
    churn()
    gfx.animation.blinker.updateAll()   -- used to write the freed blinker
    gfx.animation.blinker.stopAll()
    local keep = gfx.animation.blinker.new(10, 10, true)
    keep:start()
    gfx.animation.blinker.updateAll()
    T.ok(keep:isRunning(), "a live blinker still runs")
    gfx.animation.blinker.stopAll()
    T.ok(not keep:isRunning(), "stopAll reached the live blinker")
end)

-- ── Sprite size is bounds only (Medium row) ────────────────────────────────

T.case("oversize_sprite_reads_only_its_image", function()
    guarded(function()
        local a = gfx.sprite.new(gfx.image.new(8, 8))
        local b = gfx.sprite.new(gfx.image.new(8, 8))
        a:setSize(64, 64)
        b.width, b.height = 64, 64
        a:moveTo(0, 0)
        b:moveTo(40, 40)             -- overlaps a's bounds, not its pixels
        a:add()
        b:add()
        a:draw(0, 0)
        b:update()
        gfx.sprite.update()
        a:setScaleNN(2)
        a:draw(0, 0)
        a:setScaleNN(1)
        a:setRotation(45)
        a:draw(100, 100)
        a:setRotation(0)
        T.eq(select(1, a:getSize()), 64, "size is kept as bounds")
        T.eq(a:alphaCollision(b), false, "pixels do not overlap")
        b:moveTo(4, 4)
        T.eq(a:alphaCollision(b), true, "pixels overlap")
    end)
end)

-- ── Only the documented types are accepted (Task 7 review item 0) ──────────

T.case("loadFromBuffer_qmibuf", function()
    local data = bmp(4, 4, 0xF800)
    local q = T.ok(pc.sys.qmiPsramAlloc(#data), "qmi alloc")
    pc.sys.qmiPsramWrite(q, 0, data)
    local img = gfx.image.loadFromBuffer(q, #data)
    T.eq(select(1, img:getSize()), 4, "decoded from a qmibuf")
    img = gfx.image.loadFromBuffer(q)        -- length defaults to the size
    T.eq(select(2, img:getSize()), 4, "decoded with the default length")
    -- A length past the end of the buffer is refused, never read.
    local small = T.ok(pc.sys.qmiPsramAlloc(8), "small qmi alloc")
    pc.sys.qmiPsramWrite(small, 0, "BM\0\0\0\0\0\0")
    T.raises(function() gfx.image.loadFromBuffer(small, 4096) end)
    pc.sys.qmiPsramFree(q)
    T.raises(function() gfx.image.loadFromBuffer(q, #data) end, "freed")
    -- Any other userdata (or a table) is not a buffer.
    local s = gfx.sprite.new()
    T.raises(function() gfx.image.loadFromBuffer(s, 64) end)
    T.raises(function() gfx.image.loadFromBuffer(gfx.image.new(8, 8), 64) end)
    T.raises(function() gfx.image.loadFromBuffer({}, 64) end)
end)

T.case("sprite_new_rejects_foreign_userdata", function()
    local q = T.ok(pc.sys.qmiPsramAlloc(64), "qmi alloc")
    T.raises(function() gfx.sprite.new(q) end)
    T.raises(function() gfx.sprite.new(gfx.sprite.new()) end)
    T.raises(function() gfx.sprite.new("image.png") end)
    T.ok(gfx.sprite.new(nil), "sprite.new(nil) is an empty sprite")
end)

T.case("loop_skips_foreign_frames", function()
    local q = T.ok(pc.sys.qmiPsramAlloc(64), "qmi alloc")
    local loop = gfx.animation.loop.new(100, { q })
    T.eq(loop:image(), nil, "a qmibuf is not a frame")
    loop:draw(0, 0)
    loop:setImageTable({ gfx.sprite.new() })
    T.eq(loop:image(), nil, "a sprite is not a frame")
    loop:draw(0, 0)
end)

T.done()
