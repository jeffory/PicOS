"""Bridge metatables hide their metamethods (review: Lua High, "__index is
the metatable itself, so v:__gc() can be called from Lua").

Every bridge userdata type is registered by lb_register_type (lua_bridge.c):
methods live in a separate table reached through __index, the metatable
holds only metamethods and is locked (__metatable = false). So obj:__gc()
is "attempt to call a nil value", never a second finaliser run, and
getmetatable(obj) is false.

A destroyed object can still be reached from Lua through resurrection: a
finaliser that runs after the object's own __gc can use it. Those cases
build that order deliberately (Lua 5.4 runs finalisers newest-marked first)
and require a clean Lua error rather than a NULL dereference.

Each case is its own inline app on its own simulator, because a regression
takes the simulator down. video (hardware-only; the sim stubs the player) and
crypto (absent in the sim) are hardware-pending.
"""

import pytest

from helpers import run_lua_app, stage_lua_app, write_mod, write_wav

# name -> (Lua expression building one object, requirements)
TYPES = {
    "image": ("G.image.new(8, 8)", ()),
    "sprite": ("G.sprite.new(G.image.new(8, 8))", ()),
    "spritesheet": ("G.spritesheet.newGrid(G.image.new(8, 8), 1, 1, 8, 8)", ()),
    "tilemap": ("G.tilemap.new(G.image.new(8, 8), 8, 8)", ()),
    "animation_loop": ("G.animation.loop.new(100, {G.image.new(8, 8)})", ()),
    "animator": ("G.animation.animator.new(100, 0, 1)", ()),
    "blinker": ("G.animation.blinker.new()", ()),
    "font": ("G.font.new('6x8')", ()),
    "terminal": ("pc.terminal.new(10, 5)", ()),
    "modplayer": ("pc.modplayer.create()", ()),
    "camera": ("pc.game.camera.new()", ()),
    "scene_pool": ("pc.game.scene.objectPool('p', function() return {} end)", ()),
    "zip_archive": ("pc.zip.open(APP_DIR .. '/a.zip')", ()),
    "tcp": ("pc.tcp.new('127.0.0.1', 9)", ("http",)),
    "http": ("pc.network.http.new('127.0.0.1', 9)", ("http",)),
    "sample": ("pc.sound.sample(APP_DIR .. '/s.wav')", ("audio",)),
    "sampleplayer": ("pc.sound.sampleplayer(pc.sound.sample(APP_DIR .. '/s.wav'))",
                     ("audio",)),
    "fileplayer": ("pc.sound.fileplayer()", ("audio",)),
    "mp3player": ("pc.sound.mp3player()", ("audio",)),
    "fs_file": ("pc.fs.open(pc.fs.appPath('f.txt'), 'w')", ()),
}

PRELUDE = """
local pc = picocalc
local G = pc.graphics
local T = picocalc.sys.loadlib('picotest')
"""


def _zip_bytes():
    import io
    import zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("hello.txt", "hi")
    return buf.getvalue()


def _stage(sim, name, body, requirements=()):
    app = stage_lua_app(sim.sd_card_path, name, PRELUDE + body + "\nT.done()\n",
                        requirements=requirements,
                        files={"a.zip": _zip_bytes()})
    write_wav(app / "s.wav")
    write_mod(app / "m.mod")
    return app


HIDDEN = """
T.case('hidden', function()
    local o = T.ok(%s, 'constructor')
    T.raises(function() o:__gc() end, 'nil value')
    T.eq(o.__gc, nil, 'o.__gc')
    T.eq(o.__index, nil, 'o.__index')
    T.eq(getmetatable(o), false, 'metatable is locked')
    T.raises(function() setmetatable(o, {}) end)
    o = nil
    collectgarbage('collect'); collectgarbage('collect')
end)
"""


@pytest.mark.parametrize("name", list(TYPES))
def test_metamethods_hidden(simulator, name):
    expr, req = TYPES[name]
    _stage(simulator, f"mt_{name}", HIDDEN % expr, req)
    run = run_lua_app(simulator, f"mt_{name}", timeout=20)
    run.assert_all_passed(["hidden"])


# ── Using an object after its own finaliser ran ─────────────────────────────

# arm(make, use) creates a holder with a finaliser, then the object (so the
# object is finalised first), and has the holder's finaliser call use(obj).
# The result of that call lands in R after a full collection.
AFTER = """
local R = {}
local function arm(make, use)
    local holder = setmetatable({}, {__gc = function(h)
        R.ran = true
        R.ok, R.err = pcall(use, h.obj, h.extra)
    end})
    holder.obj, holder.extra = make()
end
local function settle()
    for _ = 1, 3 do collectgarbage('collect') end
end
"""

DESTROYED = {
    "terminal": """
        arm(function() return pc.terminal.new(10, 5) end,
            function(t) return t:getCols() end)
    """,
    "image": """
        arm(function() return G.image.new(8, 8) end,
            function(img) img:draw(0, 0) end)
    """,
    "image_under_loop": """
        -- loop is marked before the holder, the image after it: the image
        -- is finalised first, then the holder draws through the live loop.
        local loop = G.animation.loop.new(100)
        local holder = setmetatable({}, {__gc = function(h)
            R.ran = true
            R.ok, R.err = pcall(h.loop.draw, h.loop, 0, 0)
        end})
        holder.loop = loop
        loop:setImageTable({G.image.new(8, 8)})
        loop, holder = nil, nil
    """,
    "modplayer": """
        arm(function() return pc.modplayer.create() end,
            function(m) return m:getVolume() end)
    """,
    "blinker": """
        arm(function() return G.animation.blinker.new() end,
            function(b) b:start() end)
    """,
    "sprite": """
        arm(function() return G.sprite.new() end,
            function(s) s:add() end)
    """,
}


@pytest.mark.parametrize("name", list(DESTROYED))
def test_destroyed_object_errors(simulator, name):
    body = AFTER + (
        "T.case('destroyed', function()\n"
        + DESTROYED[name] +
        "    settle()\n"
        "    T.ok(R.ran, 'the holder finaliser never ran')\n"
        "    T.eq(R.ok, false, 'using a destroyed object succeeded')\n"
        "    T.ok(tostring(R.err):find('destroyed') or tostring(R.err):find('freed'),\n"
        "         'error: ' .. tostring(R.err))\n"
        "end)\n")
    _stage(simulator, f"dead_{name}", body)
    run = run_lua_app(simulator, f"dead_{name}", timeout=20)
    run.assert_all_passed(["destroyed"])
