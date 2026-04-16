-- calc_memory.lua — Memory and history management
-- Pure Lua 5.4, no PicOS dependencies. Testable on host.

local Memory = {}
Memory.__index = Memory

function Memory.new(history_cap)
    local self = setmetatable({}, Memory)
    self._value = 0
    self._has_value = false
    self._history = {}
    self._history_cap = history_cap or 50
    self._last_answer = nil
    return self
end

---------------------------------------------------------------------------
-- Memory operations
---------------------------------------------------------------------------

function Memory:store(val)
    self._value = val
    self._has_value = true
end

function Memory:recall()
    return self._value
end

function Memory:clear()
    self._value = 0
    self._has_value = false
end

function Memory:has_value()
    return self._has_value
end

function Memory:add(val)
    self._value = self._value + val
    self._has_value = true
end

function Memory:subtract(val)
    self._value = self._value - val
    self._has_value = true
end

---------------------------------------------------------------------------
-- History
---------------------------------------------------------------------------

function Memory:push_history(expr, result)
    self._history[#self._history + 1] = {expr = expr, result = result}
    self._last_answer = result
    -- Trim to cap
    while #self._history > self._history_cap do
        table.remove(self._history, 1)
    end
end

function Memory:get_history()
    return self._history
end

function Memory:get_last_answer()
    return self._last_answer
end

function Memory:clear_history()
    self._history = {}
    self._last_answer = nil
end

---------------------------------------------------------------------------
-- Serialization (minimal JSON — no external library needed)
---------------------------------------------------------------------------

local function escape_json_string(s)
    s = s:gsub('\\', '\\\\')
    s = s:gsub('"', '\\"')
    s = s:gsub('\n', '\\n')
    s = s:gsub('\r', '\\r')
    s = s:gsub('\t', '\\t')
    return s
end

function Memory:serialize()
    local parts = {}
    parts[#parts + 1] = '{"history":['
    for i, entry in ipairs(self._history) do
        if i > 1 then parts[#parts + 1] = ',' end
        parts[#parts + 1] = '{"expr":"'
        parts[#parts + 1] = escape_json_string(entry.expr)
        parts[#parts + 1] = '","result":'
        parts[#parts + 1] = tostring(entry.result)
        parts[#parts + 1] = '}'
    end
    parts[#parts + 1] = ']}'
    return table.concat(parts)
end

function Memory:deserialize(json)
    if not json or json == "" then return end
    self._history = {}
    self._last_answer = nil

    -- Minimal JSON parsing for our known format
    for expr, result in json:gmatch('"expr":"(.-)".-"result":([%d%.eE%+%-]+)') do
        local num = tonumber(result)
        if expr and num then
            self._history[#self._history + 1] = {expr = expr, result = num}
            self._last_answer = num
        end
    end
end

return Memory
