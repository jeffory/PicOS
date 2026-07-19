-- test_calc_stats.lua — Tests for statistics module
-- Run with: cd apps/calculator/tests && lua test_all.lua

local lu = require("luaunit")
local Stats = dofile("../calc_stats.lua")

---------------------------------------------------------------------------
-- Empty dataset
---------------------------------------------------------------------------

TestStatsEmpty = {}

function TestStatsEmpty:test_count_zero()
    local s = Stats.new()
    lu.assertEquals(s:count(), 0)
end

function TestStatsEmpty:test_sum_zero()
    local s = Stats.new()
    lu.assertEquals(s:sum(), 0)
end

function TestStatsEmpty:test_mean_nil()
    local s = Stats.new()
    lu.assertNil(s:mean())
end

function TestStatsEmpty:test_median_nil()
    local s = Stats.new()
    lu.assertNil(s:median())
end

function TestStatsEmpty:test_mode_nil()
    local s = Stats.new()
    lu.assertNil(s:mode())
end

function TestStatsEmpty:test_stddev_nil()
    local s = Stats.new()
    lu.assertNil(s:stddev_pop())
    lu.assertNil(s:stddev_sample())
end

---------------------------------------------------------------------------
-- Single value
---------------------------------------------------------------------------

TestStatsSingle = {}

function TestStatsSingle:test_single_mean()
    local s = Stats.new()
    s:add(5)
    lu.assertEquals(s:mean(), 5)
end

function TestStatsSingle:test_single_median()
    local s = Stats.new()
    s:add(5)
    lu.assertEquals(s:median(), 5)
end

function TestStatsSingle:test_single_stddev_pop()
    local s = Stats.new()
    s:add(5)
    lu.assertAlmostEquals(s:stddev_pop(), 0, 1e-10)
end

function TestStatsSingle:test_single_stddev_sample_nil()
    local s = Stats.new()
    s:add(5)
    lu.assertNil(s:stddev_sample())  -- need n >= 2 for sample
end

---------------------------------------------------------------------------
-- Known dataset: {2, 4, 4, 4, 5, 5, 7, 9}
---------------------------------------------------------------------------

TestStatsDataset = {}

function TestStatsDataset:setUp()
    self.s = Stats.new()
    local data = {2, 4, 4, 4, 5, 5, 7, 9}
    for _, v in ipairs(data) do self.s:add(v) end
end

function TestStatsDataset:test_count()
    lu.assertEquals(self.s:count(), 8)
end

function TestStatsDataset:test_sum()
    lu.assertEquals(self.s:sum(), 40)
end

function TestStatsDataset:test_mean()
    lu.assertAlmostEquals(self.s:mean(), 5, 1e-10)
end

function TestStatsDataset:test_median()
    lu.assertAlmostEquals(self.s:median(), 4.5, 1e-10)
end

function TestStatsDataset:test_mode()
    lu.assertEquals(self.s:mode(), 4)
end

function TestStatsDataset:test_min()
    lu.assertEquals(self.s:min(), 2)
end

function TestStatsDataset:test_max()
    lu.assertEquals(self.s:max(), 9)
end

function TestStatsDataset:test_variance_pop()
    lu.assertAlmostEquals(self.s:variance_pop(), 4, 1e-10)
end

function TestStatsDataset:test_variance_sample()
    lu.assertAlmostEquals(self.s:variance_sample(), 4 * 8 / 7, 1e-10)
end

function TestStatsDataset:test_stddev_pop()
    lu.assertAlmostEquals(self.s:stddev_pop(), 2, 1e-10)
end

function TestStatsDataset:test_stddev_sample()
    lu.assertAlmostEquals(self.s:stddev_sample(), math.sqrt(4 * 8 / 7), 1e-10)
end

---------------------------------------------------------------------------
-- Add/remove
---------------------------------------------------------------------------

TestStatsAddRemove = {}

function TestStatsAddRemove:test_add_updates_count()
    local s = Stats.new()
    s:add(10)
    s:add(20)
    lu.assertEquals(s:count(), 2)
end

function TestStatsAddRemove:test_remove_by_index()
    local s = Stats.new()
    s:add(10)
    s:add(20)
    s:add(30)
    s:remove(2)
    lu.assertEquals(s:count(), 2)
    local data = s:get_data()
    lu.assertEquals(data[1], 10)
    lu.assertEquals(data[2], 30)
end

function TestStatsAddRemove:test_clear()
    local s = Stats.new()
    s:add(1)
    s:add(2)
    s:clear()
    lu.assertEquals(s:count(), 0)
end

---------------------------------------------------------------------------
-- Median edge cases
---------------------------------------------------------------------------

TestStatsMedian = {}

function TestStatsMedian:test_odd_count()
    local s = Stats.new()
    s:add(1) s:add(3) s:add(5)
    lu.assertEquals(s:median(), 3)
end

function TestStatsMedian:test_even_count()
    local s = Stats.new()
    s:add(1) s:add(2) s:add(3) s:add(4)
    lu.assertAlmostEquals(s:median(), 2.5, 1e-10)
end

function TestStatsMedian:test_unsorted_input()
    local s = Stats.new()
    s:add(5) s:add(1) s:add(3) s:add(2) s:add(4)
    lu.assertEquals(s:median(), 3)
end

---------------------------------------------------------------------------
-- Mode edge cases
---------------------------------------------------------------------------

TestStatsMode = {}

function TestStatsMode:test_no_mode()
    local s = Stats.new()
    s:add(1) s:add(2) s:add(3)
    lu.assertNil(s:mode())  -- all equally frequent = no mode
end

function TestStatsMode:test_multiple_modes()
    local s = Stats.new()
    s:add(1) s:add(1) s:add(2) s:add(2) s:add(3)
    -- Returns the first mode found (smallest)
    local m = s:mode()
    lu.assertTrue(m == 1 or m == 2)
end

---------------------------------------------------------------------------
-- Linear regression
---------------------------------------------------------------------------

TestStatsRegression = {}

function TestStatsRegression:test_perfect_line()
    local s = Stats.new()
    s:add_xy(1, 2)
    s:add_xy(2, 4)
    s:add_xy(3, 6)
    local reg = s:linear_regression()
    lu.assertNotNil(reg)
    lu.assertAlmostEquals(reg.slope, 2, 1e-10)
    lu.assertAlmostEquals(reg.intercept, 0, 1e-10)
    lu.assertAlmostEquals(reg.r, 1.0, 1e-10)
end

function TestStatsRegression:test_negative_slope()
    local s = Stats.new()
    s:add_xy(1, 6)
    s:add_xy(2, 4)
    s:add_xy(3, 2)
    local reg = s:linear_regression()
    lu.assertAlmostEquals(reg.slope, -2, 1e-10)
    lu.assertAlmostEquals(reg.intercept, 8, 1e-10)
    lu.assertAlmostEquals(reg.r, -1.0, 1e-10)
end

function TestStatsRegression:test_insufficient_data()
    local s = Stats.new()
    s:add_xy(1, 2)
    lu.assertNil(s:linear_regression())
end
