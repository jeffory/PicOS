"""Bridge metatables hide their metamethods (review: Lua High, "__index is
the metatable itself, so v:__gc() can be called from Lua").

Every bridge userdata type is registered by lb_register_type (lua_bridge.c):
methods live in a separate table reached through __index, the metatable
holds only metamethods and is locked (__metatable = false). So obj:__gc()
is "attempt to call a nil value", never a second finaliser run, and
getmetatable(obj) is false.

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
