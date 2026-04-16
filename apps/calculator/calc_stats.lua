-- calc_stats.lua — Statistics computations
-- Pure Lua 5.4, no PicOS dependencies. Testable on host.

local Stats = {}
Stats.__index = Stats

function Stats.new()
    local self = setmetatable({}, Stats)
    self._data = {}
    self._xy_data = {}  -- for linear regression: {x, y} pairs
    return self
end

---------------------------------------------------------------------------
-- Data management
---------------------------------------------------------------------------

function Stats:add(val)
    self._data[#self._data + 1] = val
end

function Stats:add_xy(x, y)
    self._xy_data[#self._xy_data + 1] = {x = x, y = y}
end

function Stats:remove(index)
    if index >= 1 and index <= #self._data then
        table.remove(self._data, index)
    end
end

function Stats:clear()
    self._data = {}
    self._xy_data = {}
end

function Stats:get_data()
    return self._data
end

function Stats:count()
    return #self._data
end

---------------------------------------------------------------------------
-- Basic statistics
---------------------------------------------------------------------------

function Stats:sum()
    local s = 0
    for _, v in ipairs(self._data) do
        s = s + v
    end
    return s
end

function Stats:mean()
    local n = #self._data
    if n == 0 then return nil end
    return self:sum() / n
end

function Stats:min()
    if #self._data == 0 then return nil end
    local m = self._data[1]
    for i = 2, #self._data do
        if self._data[i] < m then m = self._data[i] end
    end
    return m
end

function Stats:max()
    if #self._data == 0 then return nil end
    local m = self._data[1]
    for i = 2, #self._data do
        if self._data[i] > m then m = self._data[i] end
    end
    return m
end

---------------------------------------------------------------------------
-- Median
---------------------------------------------------------------------------

function Stats:median()
    local n = #self._data
    if n == 0 then return nil end

    -- Sort a copy
    local sorted = {}
    for i, v in ipairs(self._data) do sorted[i] = v end
    table.sort(sorted)

    if n % 2 == 1 then
        return sorted[(n + 1) / 2]
    else
        return (sorted[n / 2] + sorted[n / 2 + 1]) / 2
    end
end

---------------------------------------------------------------------------
-- Mode
---------------------------------------------------------------------------

function Stats:mode()
    local n = #self._data
    if n == 0 then return nil end

    local counts = {}
    for _, v in ipairs(self._data) do
        counts[v] = (counts[v] or 0) + 1
    end

    local max_count = 0
    local mode_val = nil
    local all_same = true
    local first_count = nil

    for v, c in pairs(counts) do
        if first_count == nil then
            first_count = c
        elseif c ~= first_count then
            all_same = false
        end
        if c > max_count then
            max_count = c
            mode_val = v
        end
    end

    -- If all values appear equally often, there is no mode
    if all_same then return nil end

    return mode_val
end

---------------------------------------------------------------------------
-- Variance and standard deviation
---------------------------------------------------------------------------

function Stats:variance_pop()
    local n = #self._data
    if n == 0 then return nil end
    local m = self:mean()
    local sum_sq = 0
    for _, v in ipairs(self._data) do
        local d = v - m
        sum_sq = sum_sq + d * d
    end
    return sum_sq / n
end

function Stats:variance_sample()
    local n = #self._data
    if n < 2 then return nil end
    local m = self:mean()
    local sum_sq = 0
    for _, v in ipairs(self._data) do
        local d = v - m
        sum_sq = sum_sq + d * d
    end
    return sum_sq / (n - 1)
end

function Stats:stddev_pop()
    local v = self:variance_pop()
    if v == nil then return nil end
    return math.sqrt(v)
end

function Stats:stddev_sample()
    local v = self:variance_sample()
    if v == nil then return nil end
    return math.sqrt(v)
end

---------------------------------------------------------------------------
-- Linear regression (on xy_data)
---------------------------------------------------------------------------

function Stats:linear_regression()
    local n = #self._xy_data
    if n < 2 then return nil end

    local sum_x, sum_y, sum_xy, sum_x2, sum_y2 = 0, 0, 0, 0, 0
    for _, p in ipairs(self._xy_data) do
        sum_x = sum_x + p.x
        sum_y = sum_y + p.y
        sum_xy = sum_xy + p.x * p.y
        sum_x2 = sum_x2 + p.x * p.x
        sum_y2 = sum_y2 + p.y * p.y
    end

    local denom = n * sum_x2 - sum_x * sum_x
    if denom == 0 then return nil end  -- vertical line

    local slope = (n * sum_xy - sum_x * sum_y) / denom
    local intercept = (sum_y - slope * sum_x) / n

    -- Pearson correlation coefficient
    local num_r = n * sum_xy - sum_x * sum_y
    local den_r = math.sqrt((n * sum_x2 - sum_x * sum_x) * (n * sum_y2 - sum_y * sum_y))
    local r = 0
    if den_r > 0 then
        r = num_r / den_r
    end

    return {slope = slope, intercept = intercept, r = r}
end

return Stats
