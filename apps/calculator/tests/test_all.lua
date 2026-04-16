-- test_all.lua — Master test runner for PicOS Calculator
-- Usage: cd apps/calculator/tests && lua test_all.lua

local lu = require("luaunit")

require("test_calc_engine")
require("test_calc_functions")
require("test_calc_memory")
require("test_calc_base")
require("test_calc_stats")
require("test_calc_complex")
require("test_calc_solver")
require("test_calc_graph")

os.exit(lu.LuaUnit.run())
