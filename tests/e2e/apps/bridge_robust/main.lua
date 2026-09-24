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

T.done()
