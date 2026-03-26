-- test_buffer.lua — Unit tests for buffer.lua
-- Run with: lua test_buffer.lua

local Buffer = dofile("buffer.lua")

local pass_count = 0
local fail_count = 0
local test_name = ""

local function test(name)
    test_name = name
end

local function assert_eq(a, b, msg)
    if a ~= b then
        fail_count = fail_count + 1
        print(string.format("FAIL [%s] %s: expected %q, got %q",
            test_name, msg or "", tostring(b), tostring(a)))
    else
        pass_count = pass_count + 1
    end
end

local function assert_true(v, msg)
    if not v then
        fail_count = fail_count + 1
        print(string.format("FAIL [%s] %s", test_name, msg or "expected true"))
    else
        pass_count = pass_count + 1
    end
end

local function assert_nil(v, msg)
    if v ~= nil then
        fail_count = fail_count + 1
        print(string.format("FAIL [%s] %s: expected nil, got %q", test_name, msg or "", tostring(v)))
    else
        pass_count = pass_count + 1
    end
end

------------------------------------------------------------
-- Buffer.new
------------------------------------------------------------
test("new buffer")
local b = Buffer.new("/test.lua")
assert_eq(#b.lines, 1, "one empty line")
assert_eq(b.lines[1], "", "empty string")
assert_eq(b.cursor_x, 1, "cursor_x")
assert_eq(b.cursor_y, 1, "cursor_y")
assert_eq(b.modified, false, "not modified")
assert_eq(b.filepath, "/test.lua", "filepath")

------------------------------------------------------------
-- load_string
------------------------------------------------------------
test("load_string simple")
b = Buffer.new()
b:load_string("hello\nworld\n")
assert_eq(#b.lines, 3, "3 lines (trailing newline)")
assert_eq(b.lines[1], "hello", "line 1")
assert_eq(b.lines[2], "world", "line 2")
assert_eq(b.lines[3], "", "trailing empty line")
assert_eq(b.modified, false, "not modified after load")

test("load_string crlf")
b = Buffer.new()
b:load_string("a\r\nb\r\n")
assert_eq(b.lines[1], "a", "strips CR")
assert_eq(b.lines[2], "b", "strips CR line 2")

test("load_string no trailing newline")
b = Buffer.new()
b:load_string("abc")
assert_eq(#b.lines, 1, "1 line")
assert_eq(b.lines[1], "abc", "content")

test("load_string empty")
b = Buffer.new()
b:load_string("")
assert_eq(#b.lines, 1, "1 empty line")
assert_eq(b.lines[1], "", "empty")

------------------------------------------------------------
-- to_string
------------------------------------------------------------
test("to_string roundtrip")
b = Buffer.new()
b:load_string("line1\nline2\n")
assert_eq(b:to_string(), "line1\nline2\n\n", "roundtrip")

test("to_string single line")
b = Buffer.new()
b:load_string("hello")
assert_eq(b:to_string(), "hello\n", "adds trailing newline")

------------------------------------------------------------
-- insert_char
------------------------------------------------------------
test("insert_char basic")
b = Buffer.new()
assert_eq(b.lines[1], "", "empty initially")
b:insert_char("a")
assert_eq(b.lines[1], "a", "inserted a")
assert_eq(b.cursor_x, 2, "cursor advanced")
assert_eq(b.modified, true, "modified")

test("insert_char middle")
b = Buffer.new()
b:load_string("hllo")
b.cursor_x = 2
b:insert_char("e")
assert_eq(b.lines[1], "hello", "inserted in middle")
assert_eq(b.cursor_x, 3, "cursor at 3")

------------------------------------------------------------
-- delete_char
------------------------------------------------------------
test("delete_char basic")
b = Buffer.new()
b:load_string("abc")
b.cursor_x = 2
b:delete_char()
assert_eq(b.lines[1], "ac", "deleted b")
assert_eq(b.cursor_x, 2, "cursor unchanged")

test("delete_char joins lines")
b = Buffer.new()
b:load_string("ab\ncd")
b.cursor_x = 3  -- end of line 1
b:delete_char()
assert_eq(#b.lines, 1, "joined to 1 line")
assert_eq(b.lines[1], "abcd", "joined content")

test("delete_char at end of last line")
b = Buffer.new()
b:load_string("abc")
b.cursor_x = 4  -- past end
b:delete_char()
assert_eq(b.lines[1], "abc", "no change")

------------------------------------------------------------
-- backspace
------------------------------------------------------------
test("backspace basic")
b = Buffer.new()
b:load_string("abc")
b.cursor_x = 3
b:backspace()
assert_eq(b.lines[1], "ac", "deleted b")
assert_eq(b.cursor_x, 2, "cursor moved back")

test("backspace joins lines")
b = Buffer.new()
b:load_string("ab\ncd")
b.cursor_y = 2
b.cursor_x = 1
b:backspace()
assert_eq(#b.lines, 1, "joined")
assert_eq(b.lines[1], "abcd", "content joined")
assert_eq(b.cursor_y, 1, "on line 1")
assert_eq(b.cursor_x, 3, "at end of original line 1")

test("backspace at beginning of first line")
b = Buffer.new()
b:load_string("abc")
b.cursor_x = 1
b:backspace()
assert_eq(b.lines[1], "abc", "no change")
assert_eq(b.cursor_x, 1, "cursor unchanged")

------------------------------------------------------------
-- insert_newline
------------------------------------------------------------
test("insert_newline middle")
b = Buffer.new()
b:load_string("abcd")
b.cursor_x = 3
b:insert_newline()
assert_eq(#b.lines, 2, "2 lines")
assert_eq(b.lines[1], "ab", "before split")
assert_eq(b.lines[2], "cd", "after split")
assert_eq(b.cursor_y, 2, "cursor on line 2")
assert_eq(b.cursor_x, 1, "cursor at beginning")

test("insert_newline at start")
b = Buffer.new()
b:load_string("abc")
b.cursor_x = 1
b:insert_newline()
assert_eq(b.lines[1], "", "empty first line")
assert_eq(b.lines[2], "abc", "content moved")

test("insert_newline at end")
b = Buffer.new()
b:load_string("abc")
b.cursor_x = 4
b:insert_newline()
assert_eq(b.lines[1], "abc", "first line intact")
assert_eq(b.lines[2], "", "empty new line")

------------------------------------------------------------
-- delete_line
------------------------------------------------------------
test("delete_line")
b = Buffer.new()
b:load_string("line1\nline2\nline3")
b.cursor_y = 2
b:delete_line()
assert_eq(#b.lines, 2, "2 lines remain")
assert_eq(b.lines[1], "line1", "line1 intact")
assert_eq(b.lines[2], "line3", "line3 is now line 2")

test("delete_line last line")
b = Buffer.new()
b:load_string("only")
b:delete_line()
assert_eq(#b.lines, 1, "still 1 line")
assert_eq(b.lines[1], "", "empty line remains")

------------------------------------------------------------
-- insert_tab
------------------------------------------------------------
test("insert_tab spaces")
b = Buffer.new()
b:insert_tab(true, 4)
assert_eq(b.lines[1], "    ", "4 spaces")
assert_eq(b.cursor_x, 5, "cursor after spaces")

test("insert_tab spaces alignment")
b = Buffer.new()
b:insert_char("a")  -- cursor at 2
b:insert_tab(true, 4)
assert_eq(b.lines[1], "a   ", "3 spaces to align")

test("insert_tab literal")
b = Buffer.new()
b:insert_tab(false, 4)
assert_eq(b.lines[1], "\t", "tab char")

------------------------------------------------------------
-- Undo / Redo
------------------------------------------------------------
test("undo insert_char")
b = Buffer.new()
b:insert_char("a")
b:insert_char("b")
b:insert_char("c")
assert_eq(b.lines[1], "abc", "before undo")
b:undo()
-- Coalesced: all 3 chars in one entry
assert_eq(b.lines[1], "", "undo all coalesced chars")
assert_eq(b.cursor_x, 1, "cursor reset")

test("redo insert_char")
b = Buffer.new()
b:insert_char("x")
b:insert_char("y")
b:undo()
assert_eq(b.lines[1], "", "undone")
b:redo()
assert_eq(b.lines[1], "xy", "redone")
assert_eq(b.cursor_x, 3, "cursor at end")

test("undo delete_char")
b = Buffer.new()
b:load_string("abc")
b.cursor_x = 2
b:delete_char()
assert_eq(b.lines[1], "ac", "deleted")
b:undo()
assert_eq(b.lines[1], "abc", "restored")
assert_eq(b.cursor_x, 2, "cursor restored")

test("undo backspace")
b = Buffer.new()
b:load_string("abc")
b.cursor_x = 3
b:backspace()
assert_eq(b.lines[1], "ac", "backspaced")
b:undo()
assert_eq(b.lines[1], "abc", "restored")
assert_eq(b.cursor_x, 3, "cursor restored")

test("undo newline")
b = Buffer.new()
b:load_string("abcd")
b.cursor_x = 3
b:insert_newline()
assert_eq(#b.lines, 2, "split")
b:undo()
assert_eq(#b.lines, 1, "reunited")
assert_eq(b.lines[1], "abcd", "content restored")
assert_eq(b.cursor_x, 3, "cursor restored")

test("undo join via delete_char")
b = Buffer.new()
b:load_string("ab\ncd")
b.cursor_x = 3
b:delete_char()
assert_eq(#b.lines, 1, "joined")
b:undo()
assert_eq(#b.lines, 2, "split again")
assert_eq(b.lines[1], "ab", "line 1")
assert_eq(b.lines[2], "cd", "line 2")

test("undo join_up via backspace")
b = Buffer.new()
b:load_string("ab\ncd")
b.cursor_y = 2
b.cursor_x = 1
b:backspace()
assert_eq(#b.lines, 1, "joined")
b:undo()
assert_eq(#b.lines, 2, "split again")
assert_eq(b.cursor_y, 2, "cursor on line 2")
assert_eq(b.cursor_x, 1, "cursor at start")

test("undo delete_line")
b = Buffer.new()
b:load_string("aaa\nbbb\nccc")
b.cursor_y = 2
b:delete_line()
assert_eq(#b.lines, 2, "deleted")
b:undo()
assert_eq(#b.lines, 3, "restored")
assert_eq(b.lines[2], "bbb", "line restored")

test("undo clears redo")
b = Buffer.new()
b:insert_char("a")
b:undo()
assert_eq(#b.redo_stack, 1, "redo has entry")
b:insert_char("b")
assert_eq(#b.redo_stack, 0, "redo cleared")

test("undo on empty stack")
b = Buffer.new()
assert_eq(b:undo(), false, "returns false")

test("redo on empty stack")
b = Buffer.new()
assert_eq(b:redo(), false, "returns false")

------------------------------------------------------------
-- Coalescing
------------------------------------------------------------
test("coalesce inserts break on move")
b = Buffer.new()
b:insert_char("a")
b:insert_char("b")
b:move_cursor(0, 0)  -- no actual move, but triggers clamp
-- Insert more (after move, coalescing should break due to non-adjacent)
b:move_cursor(-1, 0)  -- move left
b:insert_char("x")
-- Should have 2 undo entries: "ab" and "x"
assert_eq(#b.undo_stack, 2, "2 undo entries")

test("coalesce backspaces")
b = Buffer.new()
b:load_string("abcdef")
b.cursor_x = 7  -- end
b:backspace()  -- delete f
b:backspace()  -- delete e
b:backspace()  -- delete d
assert_eq(b.lines[1], "abc", "3 chars deleted")
-- Should be coalesced into 1 entry
assert_eq(#b.undo_stack, 1, "1 coalesced entry")
b:undo()
assert_eq(b.lines[1], "abcdef", "all restored")

------------------------------------------------------------
-- Cursor movement
------------------------------------------------------------
test("move_cursor basic")
b = Buffer.new()
b:load_string("hello\nworld")
b:move_cursor(3, 0)
assert_eq(b.cursor_x, 4, "moved right 3")
b:move_cursor(0, 1)
assert_eq(b.cursor_y, 2, "moved down")

test("move_cursor wrap right")
b = Buffer.new()
b:load_string("ab\ncd")
b.cursor_x = 3  -- end of line 1
b:move_cursor(1, 0)  -- wrap to next line
assert_eq(b.cursor_y, 2, "wrapped to line 2")
assert_eq(b.cursor_x, 1, "at start")

test("move_cursor wrap left")
b = Buffer.new()
b:load_string("ab\ncd")
b.cursor_y = 2
b.cursor_x = 1
b:move_cursor(-1, 0)  -- wrap to prev line
assert_eq(b.cursor_y, 1, "wrapped to line 1")
assert_eq(b.cursor_x, 3, "at end")

test("clamp_cursor bounds")
b = Buffer.new()
b:load_string("abc")
b.cursor_x = 100
b:clamp_cursor()
assert_eq(b.cursor_x, 4, "clamped to len+1")
b.cursor_x = -5
b:clamp_cursor()
assert_eq(b.cursor_x, 1, "clamped to 1")
b.cursor_y = 50
b:clamp_cursor()
assert_eq(b.cursor_y, 1, "clamped to total lines")

------------------------------------------------------------
-- Scroll
------------------------------------------------------------
test("update_scroll vertical")
b = Buffer.new()
b:load_string("1\n2\n3\n4\n5\n6\n7\n8\n9\n10")
b.cursor_y = 8
b:update_scroll(5, 40)
assert_eq(b.scroll_y, 3, "scrolled down")

test("update_scroll horizontal")
b = Buffer.new()
b:load_string(string.rep("x", 100))
b.cursor_x = 50
b:update_scroll(5, 20)
assert_eq(b.scroll_x, 30, "scrolled right")

------------------------------------------------------------
-- Find
------------------------------------------------------------
test("find_next basic")
b = Buffer.new()
b:load_string("hello world\nfoo bar\nhello again")
local y, x = b:find_next("hello", 1, 1)
assert_eq(y, 1, "found on line 1")
assert_eq(x, 1, "at col 1")

test("find_next from offset")
b = Buffer.new()
b:load_string("hello world\nfoo bar\nhello again")
local y, x = b:find_next("hello", 1, 2)
assert_eq(y, 3, "found on line 3")
assert_eq(x, 1, "at col 1")

test("find_next wrap around")
b = Buffer.new()
b:load_string("abc\ndef\nghi")
local y, x = b:find_next("abc", 3, 1)
assert_eq(y, 1, "wrapped to line 1")
assert_eq(x, 1, "at col 1")

test("find_next not found")
b = Buffer.new()
b:load_string("abc\ndef")
local y, x = b:find_next("xyz", 1, 1)
assert_nil(y, "not found")

test("find_next empty needle")
b = Buffer.new()
b:load_string("abc")
local y, x = b:find_next("", 1, 1)
assert_nil(y, "empty needle returns nil")

------------------------------------------------------------
-- Replace
------------------------------------------------------------
test("replace_at basic")
b = Buffer.new()
b:load_string("hello world")
local ok = b:replace_at(1, 7, "world", "lua")
assert_true(ok, "replaced")
assert_eq(b.lines[1], "hello lua", "content replaced")

test("replace_at mismatch")
b = Buffer.new()
b:load_string("hello world")
local ok = b:replace_at(1, 7, "xyz", "lua")
assert_true(not ok, "not replaced")
assert_eq(b.lines[1], "hello world", "unchanged")

------------------------------------------------------------
-- Function scanning
------------------------------------------------------------
test("scan_functions")
b = Buffer.new()
b:load_string([[
function hello()
end
local function world(a, b)
end
function Foo.bar(x)
end
]])
local funcs = b:scan_functions()
assert_eq(#funcs, 3, "3 functions")
assert_eq(funcs[1], "Foo.bar", "sorted first")
assert_eq(funcs[2], "hello", "sorted second")
assert_eq(funcs[3], "world", "sorted third")

------------------------------------------------------------
-- Max undo limit
------------------------------------------------------------
test("undo stack limit")
b = Buffer.new()
for i = 1, 250 do
    b.cursor_x = 1  -- break coalescing by moving cursor
    b:move_cursor(0, 0)
    b:insert_char(string.char(65 + (i % 26)))
    -- Force break coalescing between each
    b._coalesce = nil
end
assert_true(#b.undo_stack <= 200, "stack capped at 200")

------------------------------------------------------------
-- Modified flag
------------------------------------------------------------
test("modified flag resets on full undo")
b = Buffer.new()
b:load_string("test")
assert_eq(b.modified, false, "not modified after load")
b:insert_char("x")
assert_eq(b.modified, true, "modified after insert")
b:undo()
assert_eq(b.modified, false, "not modified after full undo")

------------------------------------------------------------
-- Results
------------------------------------------------------------
print(string.format("\n%d passed, %d failed", pass_count, fail_count))
if fail_count > 0 then
    os.exit(1)
end
