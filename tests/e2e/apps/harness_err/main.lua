-- Harness fixture: a runtime error two calls deep (for the traceback).
picocalc.sys.log("H:ERR_START")
local function inner() error("harness boom 42") end
local function outer() inner() end
outer()
