-- Lua bridge robustness cases (review: Lua REPL / terminal.new / leak rows).
local T = picocalc.sys.loadlib("picotest")
local pc = picocalc

-- ── REPL ─────────────────────────────────────────────────────────────────
-- repl.print used to format into a 256-byte stack buffer with
-- pos += snprintf(...): past 256 the size argument wrapped and the next
-- value was written beyond the buffer.
T.case("repl_print_many_values", function()
    local t = {}
    for i = 1, 60 do t[i] = {} end              -- "table: 0x..." each
    pc.repl.print(table.unpack(t))
    local n = {}
    for i = 1, 100 do n[i] = -2147483647 - i % 2 end
    pc.repl.print(table.unpack(n))
    local f = {}
    for i = 1, 100 do f[i] = i + 0.125 end
    pc.repl.print(table.unpack(f))
end)

T.case("repl_print_long_strings", function()
    pc.repl.print(string.rep("a", 1000), string.rep("b", 300), nil, true)
end)

T.case("repl_scrollback_is_bounded", function()
    for i = 1, 600 do pc.repl.print("line " .. i) end
    pc.repl.clear()
    pc.repl.print("after clear")
    pc.repl.clear()
    pc.repl.clear()                             -- clear with nothing allocated
end)

T.case("repl_readline_idle", function()
    T.eq(pc.repl.readline(), nil)
    pc.repl.echo(false)
    pc.repl.echo(true)
end)

-- ── terminal.new ─────────────────────────────────────────────────────────
-- cols/rows were unchecked: 0 or negative silently became the default and
-- cols*rows*2 could wrap (32-bit size_t on the device).
T.case("terminal_new_rejects_bad_sizes", function()
    T.raises(function() pc.terminal.new(1000000, 1000000) end, "cols")
    T.raises(function() pc.terminal.new(0, 10) end, "cols")
    T.raises(function() pc.terminal.new(-1, 10) end, "cols")
    T.raises(function() pc.terminal.new(54, 10) end, "cols")
    T.raises(function() pc.terminal.new(10, 0) end, "rows")
    T.raises(function() pc.terminal.new(10, 27) end, "rows")
    T.raises(function() pc.terminal.new(200, 200) end, "cols")
end)

T.case("terminal_new_accepts_screen_sizes", function()
    local a = T.ok(pc.terminal.new(53, 26, 5000))
    local b = T.ok(pc.terminal.new(1, 1, 0))
    local c = T.ok(pc.terminal.new())
    a:write("x"); b:write("y"); c:write("z")
    a, b, c = nil, nil, nil
    collectgarbage("collect")
end)

-- ── Protected callbacks leave nothing on the stack ────────────────────────
-- fs.copy's progress trampoline lua_pcall'd the callback and never popped
-- the error: one stack slot per 4 KB chunk, until the stack hit
-- LUAI_MAXSTACK and the remaining callbacks failed with "stack overflow".
T.case("fs_copy_failing_progress_callback", function()
    local dir = "/data/" .. APP_ID
    local src, dst = dir .. "/big.bin", dir .. "/big_copy.bin"
    local f = T.ok(pc.fs.open(src, "w"))
    local chunk = string.rep("x", 4096)
    local N = 1100  -- more chunks than LUAI_MAXSTACK (1000) slots
    for i = 1, N do pc.fs.write(f, chunk) end
    pc.fs.close(f)
    local calls = 0
    local ok = pc.fs.copy(src, dst, function(done, total)
        calls = calls + 1
        error("progress callback failed")
    end)
    T.ok(ok, "copy failed")
    T.eq(calls, N, "progress callbacks lost (stack full)")
    T.eq(pc.fs.size(dst), N * 4096)
    pc.fs.delete(src)
    pc.fs.delete(dst)
end)

-- ── Sound handles ────────────────────────────────────────────────────────
-- There is one MP3 player: mp3player() returned a new handle to it every
-- call, and collecting any of them stopped the player under the others.
T.case("mp3player_single_handle", function()
    local a = pc.sound.mp3player()
    local b = pc.sound.mp3player()
    T.ok(rawequal(a, b), "two handles for the one MP3 player")
end)

T.case("mp3player_collected_second_handle_keeps_music", function()
    local a = T.ok(pc.sound.mp3player())
    T.ok(a:load(APP_DIR .. "/long.mp3"), "load long.mp3")
    a:play()
    T.ok(a:isPlaying(), "not playing after play()")
    do local b = pc.sound.mp3player() end
    collectgarbage("collect")
    collectgarbage("collect")
    T.ok(a:isPlaying(), "collecting a second handle stopped the music")
    a:stop()
end)

-- A collected fileplayer kept its callback slots (two per kind), so the third
-- fileplayer ever given a callback failed with "too many ... callbacks".
T.case("fileplayer_callback_slots_released_on_gc", function()
    for i = 1, 4 do
        local fp = T.ok(pc.sound.fileplayer(), "fileplayer " .. i)
        fp:setFinishCallback(function() end)
        fp:setLoopCallback(function() end)
        fp = nil
        collectgarbage("collect")
        collectgarbage("collect")
    end
    local fp = T.ok(pc.sound.fileplayer())
    fp:setFinishCallback(function() end)
    fp:setLoopCallback(function() end)
end)

T.done()
