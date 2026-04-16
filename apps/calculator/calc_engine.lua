-- calc_engine.lua — Expression tokenizer + shunting-yard evaluator
-- Pure Lua 5.4, no PicOS dependencies. Testable on host.

local Engine = {}
Engine.__index = Engine

function Engine.new()
    local self = setmetatable({}, Engine)
    self.angle_mode = "deg"   -- "deg", "rad", "grad"
    self.number_base = 10
    self.complex_mode = false
    self._Complex = nil       -- set when complex mode is enabled
    self.functions = {}       -- name -> {arity, impl}
    self.complex_functions = {} -- name -> {arity, impl} (complex-mode overrides)
    self.constants = {}       -- name -> value
    self.variables = {}       -- name -> func() returning value
    -- Register built-in constants
    self.constants["pi"] = math.pi
    self.constants["e"] = math.exp(1)
    return self
end

function Engine:set_angle_mode(mode)
    self.angle_mode = mode
end

function Engine:set_number_base(base)
    self.number_base = base
end

function Engine:set_complex_mode(enabled, Complex)
    self.complex_mode = enabled
    self._Complex = Complex
end

function Engine:register_complex_function(name, arity, impl)
    self.complex_functions[name] = {arity = arity, impl = impl}
end

function Engine:register_function(name, arity, impl)
    self.functions[name] = {arity = arity, impl = impl}
end

function Engine:register_constant(name, value)
    self.constants[name] = value
end

function Engine:register_variable(name, getter)
    self.variables[name] = getter
end

---------------------------------------------------------------------------
-- Tokenizer
---------------------------------------------------------------------------

local function is_digit(c)
    return c >= "0" and c <= "9"
end

local function is_alpha(c)
    return (c >= "a" and c <= "z") or (c >= "A" and c <= "Z") or c == "_"
end

local function is_alnum(c)
    return is_alpha(c) or is_digit(c)
end

local function is_space(c)
    return c == " " or c == "\t" or c == "\n" or c == "\r"
end

function Engine:tokenize(expr)
    if not expr or expr == "" then
        return nil, "Empty expression"
    end

    local tokens = {}
    local i = 1
    local len = #expr

    while i <= len do
        local c = expr:sub(i, i)

        -- Skip whitespace
        if is_space(c) then
            i = i + 1

        -- Number: digit or leading decimal point
        elseif is_digit(c) or (c == "." and i + 1 <= len and is_digit(expr:sub(i + 1, i + 1))) then
            local start = i
            while i <= len and is_digit(expr:sub(i, i)) do
                i = i + 1
            end
            if i <= len and expr:sub(i, i) == "." then
                i = i + 1
                while i <= len and is_digit(expr:sub(i, i)) do
                    i = i + 1
                end
            end
            local num = tonumber(expr:sub(start, i - 1))
            tokens[#tokens + 1] = {type = "number", value = num}

        -- Identifier: function name or constant
        elseif is_alpha(c) then
            local start = i
            while i <= len and is_alnum(expr:sub(i, i)) do
                i = i + 1
            end
            local name = expr:sub(start, i - 1)

            -- Check if it's followed by '(' -> function call
            -- Skip whitespace first
            local j = i
            while j <= len and is_space(expr:sub(j, j)) do
                j = j + 1
            end

            if j <= len and expr:sub(j, j) == "(" then
                tokens[#tokens + 1] = {type = "func", value = name}
            elseif self.constants[name] or self.variables[name] then
                tokens[#tokens + 1] = {type = "const", value = name}
            else
                -- Could be a function without parens or unknown — treat as const
                -- and let the evaluator catch unknown names
                tokens[#tokens + 1] = {type = "const", value = name}
            end

        -- Operators
        elseif c == "+" or c == "-" then
            -- Determine if unary or binary
            local prev = tokens[#tokens]
            if prev == nil
                or prev.type == "op"
                or prev.type == "unary"
                or prev.type == "lparen"
                or prev.type == "comma" then
                tokens[#tokens + 1] = {type = "unary", value = c}
            else
                tokens[#tokens + 1] = {type = "op", value = c}
            end
            i = i + 1

        elseif c == "*" or c == "/" or c == "%" or c == "^" then
            tokens[#tokens + 1] = {type = "op", value = c}
            i = i + 1

        elseif c == "(" then
            tokens[#tokens + 1] = {type = "lparen"}
            i = i + 1

        elseif c == ")" then
            tokens[#tokens + 1] = {type = "rparen"}
            i = i + 1

        elseif c == "," then
            tokens[#tokens + 1] = {type = "comma"}
            i = i + 1

        else
            return nil, "Invalid character: " .. c
        end
    end

    if #tokens == 0 then
        return nil, "Empty expression"
    end

    return tokens
end

---------------------------------------------------------------------------
-- Shunting-Yard Evaluator
---------------------------------------------------------------------------

local OP_PREC = {
    ["+"] = 1, ["-"] = 1,
    ["*"] = 2, ["/"] = 2, ["%"] = 2,
    ["^"] = 4,
}

local OP_RIGHT_ASSOC = {
    ["^"] = true,
}

-- Unary minus has higher precedence than binary +/- but lower than ^
local UNARY_PREC = 3

function Engine:evaluate(expr)
    local tokens, err = self:tokenize(expr)
    if not tokens then
        return nil, err or "Invalid expression"
    end

    -- Shunting-yard: convert to postfix (RPN) and evaluate
    local output = {}   -- output queue (values and operators)
    local ops = {}      -- operator stack
    local arity_stack = {} -- track function arity at each nesting level

    for idx = 1, #tokens do
        local tok = tokens[idx]

        if tok.type == "number" then
            output[#output + 1] = {type = "number", value = tok.value}
            if #arity_stack > 0 and arity_stack[#arity_stack] == 0 then
                arity_stack[#arity_stack] = 1
            end

        elseif tok.type == "const" then
            local val = self.constants[tok.value]
            if not val and self.variables[tok.value] then
                val = self.variables[tok.value]()
            end
            if val == nil then
                return nil, "Unknown constant: " .. tok.value
            end
            output[#output + 1] = {type = "number", value = val}
            if #arity_stack > 0 and arity_stack[#arity_stack] == 0 then
                arity_stack[#arity_stack] = 1
            end

        elseif tok.type == "func" then
            if not self.functions[tok.value] then
                return nil, "Unknown function: " .. tok.value
            end
            ops[#ops + 1] = {type = "func", value = tok.value}
            arity_stack[#arity_stack + 1] = 0  -- incremented when first arg seen

        elseif tok.type == "comma" then
            -- Pop operators until we hit lparen
            while #ops > 0 and ops[#ops].type ~= "lparen" do
                output[#output + 1] = ops[#ops]
                ops[#ops] = nil
            end
            if #ops == 0 then
                return nil, "Mismatched parentheses or comma"
            end
            -- Increment arity counter
            if #arity_stack > 0 then
                arity_stack[#arity_stack] = arity_stack[#arity_stack] + 1
            end

        elseif tok.type == "unary" then
            ops[#ops + 1] = {type = "unary", value = tok.value, prec = UNARY_PREC}

        elseif tok.type == "op" then
            local prec = OP_PREC[tok.value]
            local right = OP_RIGHT_ASSOC[tok.value]
            while #ops > 0 do
                local top = ops[#ops]
                if top.type == "lparen" then break end
                local top_prec
                if top.type == "unary" then
                    top_prec = UNARY_PREC
                elseif top.type == "op" then
                    top_prec = OP_PREC[top.value]
                else
                    break
                end
                if (not right and top_prec >= prec) or (right and top_prec > prec) then
                    output[#output + 1] = top
                    ops[#ops] = nil
                else
                    break
                end
            end
            ops[#ops + 1] = {type = "op", value = tok.value}

        elseif tok.type == "lparen" then
            ops[#ops + 1] = {type = "lparen"}

        elseif tok.type == "rparen" then
            -- Pop until matching lparen
            local found = false
            while #ops > 0 do
                local top = ops[#ops]
                if top.type == "lparen" then
                    ops[#ops] = nil
                    found = true
                    break
                end
                output[#output + 1] = top
                ops[#ops] = nil
            end
            if not found then
                return nil, "Mismatched parentheses"
            end
            -- If top of ops is a function, pop it
            if #ops > 0 and ops[#ops].type == "func" then
                local func_tok = ops[#ops]
                local arity = 1
                if #arity_stack > 0 then
                    arity = arity_stack[#arity_stack]
                    arity_stack[#arity_stack] = nil
                end
                func_tok.arity = arity
                output[#output + 1] = func_tok
                ops[#ops] = nil
                -- Nested function result counts as an argument to outer function
                if #arity_stack > 0 and arity_stack[#arity_stack] == 0 then
                    arity_stack[#arity_stack] = 1
                end
            end
        end
    end

    -- Pop remaining operators
    while #ops > 0 do
        local top = ops[#ops]
        if top.type == "lparen" then
            return nil, "Mismatched parentheses"
        end
        output[#output + 1] = top
        ops[#ops] = nil
    end

    -- Evaluate RPN
    local stack = {}
    local C = self.complex_mode and self._Complex or nil

    for idx = 1, #output do
        local item = output[idx]

        if item.type == "number" then
            if C then
                stack[#stack + 1] = C.is(item.value) and item.value or C.from_real(item.value)
            else
                stack[#stack + 1] = item.value
            end

        elseif item.type == "unary" then
            if #stack < 1 then
                return nil, "Invalid expression"
            end
            local a = stack[#stack]
            stack[#stack] = nil
            if item.value == "-" then
                stack[#stack + 1] = C and C.neg(a) or -a
            else
                stack[#stack + 1] = a  -- unary + is identity
            end

        elseif item.type == "op" then
            if #stack < 2 then
                return nil, "Invalid expression"
            end
            local b = stack[#stack]; stack[#stack] = nil
            local a = stack[#stack]; stack[#stack] = nil
            local r

            if C then
                if item.value == "+" then r = C.add(a, b)
                elseif item.value == "-" then r = C.sub(a, b)
                elseif item.value == "*" then r = C.mul(a, b)
                elseif item.value == "/" then
                    local bz = C.ensure(b)
                    if bz.re == 0 and bz.im == 0 then return nil, "Division by zero" end
                    r = C.div(a, b)
                elseif item.value == "%" then
                    -- Modulo only meaningful for real; extract real parts
                    local ar, br = C.ensure(a), C.ensure(b)
                    if br.re == 0 then return nil, "Division by zero" end
                    r = C.from_real(ar.re % br.re)
                elseif item.value == "^" then
                    r = C.pow(a, b)
                end
            else
                if item.value == "+" then r = a + b
                elseif item.value == "-" then r = a - b
                elseif item.value == "*" then r = a * b
                elseif item.value == "/" then
                    if b == 0 then return nil, "Division by zero" end
                    r = a / b
                elseif item.value == "%" then
                    if b == 0 then return nil, "Division by zero" end
                    r = a % b
                elseif item.value == "^" then
                    r = a ^ b
                end
            end
            stack[#stack + 1] = r

        elseif item.type == "func" then
            -- In complex mode, prefer complex_functions override
            local fdef
            if C and self.complex_functions[item.value] then
                fdef = self.complex_functions[item.value]
            else
                fdef = self.functions[item.value]
            end
            if not fdef then
                return nil, "Unknown function: " .. item.value
            end
            local arity = item.arity or fdef.arity
            if #stack < arity then
                return nil, "Not enough arguments for " .. item.value
            end
            local args = {}
            for j = arity, 1, -1 do
                args[j] = stack[#stack]
                stack[#stack] = nil
            end
            local ok, result_or_err = pcall(fdef.impl, table.unpack(args))
            if not ok then
                return nil, result_or_err
            end
            if result_or_err == nil then
                return nil, "Function " .. item.value .. " returned nil"
            end
            stack[#stack + 1] = result_or_err
        end
    end

    if #stack ~= 1 then
        return nil, "Invalid expression"
    end

    local result = stack[1]

    -- Check for NaN / unwrap pure-real complex
    if C and C.is(result) then
        if result.re ~= result.re or result.im ~= result.im then
            return nil, "Result is undefined"
        end
        -- Unwrap to real if imaginary part is effectively zero
        if math.abs(result.im) < 1e-14 then
            result = result.re
        end
    elseif type(result) == "number" and result ~= result then
        return nil, "Result is undefined"
    end

    return result
end

---------------------------------------------------------------------------
-- Result formatting
---------------------------------------------------------------------------

function Engine:format_result(n)
    local C = self._Complex
    if C and C.is(n) then
        return C.format(n)
    end
    if n ~= n then
        return "Error"
    end
    if n == math.huge then
        return "Infinity"
    end
    if n == -math.huge then
        return "-Infinity"
    end

    -- Check if it's an integer
    if n == math.floor(n) and math.abs(n) < 1e15 then
        return string.format("%.0f", n)
    end

    -- For very large or very small numbers, use scientific notation
    local abs_n = math.abs(n)
    if abs_n >= 1e10 or (abs_n > 0 and abs_n < 1e-6) then
        local s = string.format("%.6e", n)
        -- Strip trailing zeros in mantissa
        s = s:gsub("(%.[0-9]-)(0+)e", "%1e")
        s = s:gsub("%.e", "e")
        return s
    end

    -- Regular decimal
    local s = string.format("%.10g", n)
    return s
end

return Engine
