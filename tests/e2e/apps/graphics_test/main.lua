-- E2E fixture for picocalc.graphics.
--
-- Tilemap: builds a 64x64 tileset (four 32x32 tiles: red, green, blue,
-- yellow) as an in-memory 16bpp top-down BMP, puts it in a 2x2 tilemap in
-- REVERSED order (4,3 / 2,1) and draws it at the screen origin. The test
-- probes the centre of each on-screen tile. Every tile except tile 1 has a
-- non-zero source offset, so a swapped image/region argument in
-- tilemap_draw shows up as missing (black) tiles.

local pc  = picocalc
local d   = pc.display
local gfx = pc.graphics
local log = pc.sys.log

local RED, GREEN, BLUE, YELLOW = 0xF800, 0x07E0, 0x001F, 0xFFE0
local TS, TILE = 64, 32

local function tileset_bmp()
    local px = {}
    for y = 0, TS - 1 do
        for x = 0, TS - 1 do
            local c
            if y < TILE then c = (x < TILE) and RED or GREEN
            else             c = (x < TILE) and BLUE or YELLOW end
            px[#px + 1] = string.pack("<I2", c)
        end
    end
    local pixels = table.concat(px)
    local header = string.pack("<c2I4I2I2I4", "BM", 54 + #pixels, 0, 0, 54)
    local info = string.pack("<I4i4i4I2I2I4I4i4i4I4I4",
        40, TS, -TS, 1, 16, 0, #pixels, 2835, 2835, 0, 0)
    return header .. info .. pixels
end

d.clear(d.BLACK)

local ok, err = pcall(function()
    local tileset = gfx.image.loadFromBuffer(tileset_bmp())
    local tm = gfx.tilemap.new(tileset, TILE, TILE)
    tm:setSize(2, 2)
    tm:setTileAtPosition(0, 0, 4)
    tm:setTileAtPosition(1, 0, 3)
    tm:setTileAtPosition(0, 1, 2)
    tm:setTileAtPosition(1, 1, 1)
    tm:draw(0, 0)
    d.flush()
    _G.__keep = { tileset, tm }   -- tilemap holds a raw pointer to the image
end)
if ok then log("GT TILEMAP_READY") else log("GT TILEMAP_ERR " .. tostring(err)) end

log("GT DONE")
-- Hold the frame for pixel probes until ESC.
local input = pc.input
while true do
    input.update()
    if input.getButtonsPressed() & input.BTN_ESC ~= 0 then return end
    pc.sys.sleep(16)
end
