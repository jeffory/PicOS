-- tokenizer.lua — Lua syntax tokenizer for PicoForge
-- Pure Lua module, no picocalc dependencies (unit-testable on host)

local Tokenizer = {}

-- Token types
Tokenizer.KEYWORD   = "keyword"
Tokenizer.STRING    = "string"
Tokenizer.COMMENT   = "comment"
Tokenizer.NUMBER    = "number"
Tokenizer.FUNCNAME  = "funcname"
Tokenizer.API       = "api"
Tokenizer.DEFAULT   = "default"

-- Lua 5.4 keywords
local keywords = {}
for _, k in ipairs({
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or",
    "repeat", "return", "then", "true", "until", "while"
}) do
    keywords[k] = true
end

-- PicOS API prefixes
local api_prefixes = {
    "picocalc", "pc", "disp", "fs", "input",
    "audio", "wifi", "config", "tcp", "ui"
}

local function is_api_word(word)
    for _, prefix in ipairs(api_prefixes) do
        if word == prefix or word:sub(1, #prefix + 1) == prefix .. "." then
            return true
        end
    end
    return false
end

------------------------------------------------------------
-- tokenize_line(line, entry_state)
-- Returns: tokens_array, exit_state
-- tokens_array: {{start_col, end_col, token_type}, ...}
-- entry_state: nil, "block_comment", "block_string", "block_string_eq"
-- (block_string_eq stores the number of = signs)
------------------------------------------------------------

function Tokenizer.tokenize_line(line, entry_state)
    local tokens = {}
    local len = #line
    local pos = 1

    local function add(start_col, end_col, ttype)
        tokens[#tokens + 1] = {start_col, end_col, ttype}
    end

    -- Continue multi-line block comment
    if entry_state == "block_comment" or (type(entry_state) == "string" and entry_state:sub(1, 13) == "block_comment") then
        local eq_count = 0
        if type(entry_state) == "string" and #entry_state > 13 then
            eq_count = tonumber(entry_state:sub(14)) or 0
        end
        local close = "]" .. string.rep("=", eq_count) .. "]"
        local close_pos = line:find(close, 1, true)
        if close_pos then
            add(1, close_pos + #close - 1, Tokenizer.COMMENT)
            pos = close_pos + #close
            entry_state = nil
        else
            add(1, len, Tokenizer.COMMENT)
            return tokens, entry_state
        end
    end

    -- Continue multi-line block string
    if entry_state and type(entry_state) == "string" and entry_state:sub(1, 12) == "block_string" then
        local eq_count = 0
        if #entry_state > 12 then
            eq_count = tonumber(entry_state:sub(13)) or 0
        end
        local close = "]" .. string.rep("=", eq_count) .. "]"
        local close_pos = line:find(close, 1, true)
        if close_pos then
            add(1, close_pos + #close - 1, Tokenizer.STRING)
            pos = close_pos + #close
            entry_state = nil
        else
            add(1, len, Tokenizer.STRING)
            return tokens, entry_state
        end
    end

    while pos <= len do
        local ch = line:sub(pos, pos)

        -- Whitespace: skip
        if ch == " " or ch == "\t" then
            pos = pos + 1

        -- Line comment: --
        elseif ch == "-" and line:sub(pos + 1, pos + 1) == "-" then
            -- Check for block comment: --[[ or --[==[
            local bracket = line:match("^%[(=*)%[", pos + 2)
            if bracket then
                local eq_count = #bracket
                local close = "]" .. string.rep("=", eq_count) .. "]"
                local close_pos = line:find(close, pos + 4 + eq_count, true)
                if close_pos then
                    add(pos, close_pos + #close - 1, Tokenizer.COMMENT)
                    pos = close_pos + #close
                else
                    add(pos, len, Tokenizer.COMMENT)
                    local state = "block_comment"
                    if eq_count > 0 then state = state .. tostring(eq_count) end
                    return tokens, state
                end
            else
                add(pos, len, Tokenizer.COMMENT)
                return tokens, nil
            end

        -- Block string: [[ or [==[
        elseif ch == "[" then
            local bracket = line:match("^%[(=*)%[", pos)
            if bracket then
                local eq_count = #bracket
                local close = "]" .. string.rep("=", eq_count) .. "]"
                local open_len = 2 + eq_count
                local close_pos = line:find(close, pos + open_len, true)
                if close_pos then
                    add(pos, close_pos + #close - 1, Tokenizer.STRING)
                    pos = close_pos + #close
                else
                    add(pos, len, Tokenizer.STRING)
                    local state = "block_string"
                    if eq_count > 0 then state = state .. tostring(eq_count) end
                    return tokens, state
                end
            else
                pos = pos + 1
            end

        -- Strings: single or double quote
        elseif ch == '"' or ch == "'" then
            local quote = ch
            local start = pos
            pos = pos + 1
            while pos <= len do
                local c = line:sub(pos, pos)
                if c == "\\" then
                    pos = pos + 2  -- skip escape
                elseif c == quote then
                    pos = pos + 1
                    break
                else
                    pos = pos + 1
                end
            end
            add(start, pos - 1, Tokenizer.STRING)

        -- Numbers: digits, hex (0x), floats
        elseif ch:match("%d") or (ch == "." and line:sub(pos + 1, pos + 1):match("%d")) then
            local start = pos
            if ch == "0" and line:sub(pos + 1, pos + 1):match("[xX]") then
                pos = pos + 2
                while pos <= len and line:sub(pos, pos):match("[%da-fA-F_]") do pos = pos + 1 end
            else
                while pos <= len and line:sub(pos, pos):match("[%d_]") do pos = pos + 1 end
                if pos <= len and line:sub(pos, pos) == "." then
                    pos = pos + 1
                    while pos <= len and line:sub(pos, pos):match("[%d_]") do pos = pos + 1 end
                end
                if pos <= len and line:sub(pos, pos):match("[eE]") then
                    pos = pos + 1
                    if pos <= len and line:sub(pos, pos):match("[%+%-]") then pos = pos + 1 end
                    while pos <= len and line:sub(pos, pos):match("[%d_]") do pos = pos + 1 end
                end
            end
            add(start, pos - 1, Tokenizer.NUMBER)

        -- Identifiers and keywords
        elseif ch:match("[%a_]") then
            local start = pos
            while pos <= len and line:sub(pos, pos):match("[%w_]") do pos = pos + 1 end
            -- Allow dotted identifiers for API detection
            while pos <= len and line:sub(pos, pos) == "." and
                  pos + 1 <= len and line:sub(pos + 1, pos + 1):match("[%a_]") do
                pos = pos + 1
                while pos <= len and line:sub(pos, pos):match("[%w_]") do pos = pos + 1 end
            end
            local word = line:sub(start, pos - 1)
            local base = word:match("^([%a_][%w_]*)") or word

            if keywords[word] then
                -- Check if this is "function" followed by a name
                if word == "function" then
                    add(start, pos - 1, Tokenizer.KEYWORD)
                    -- Skip whitespace and capture function name
                    local rest = line:sub(pos)
                    local ws, fname = rest:match("^(%s+)([%a_][%w_.]*)")
                    if fname and not keywords[fname] then
                        add(pos + #ws, pos + #ws + #fname - 1, Tokenizer.FUNCNAME)
                        pos = pos + #ws + #fname
                    end
                else
                    add(start, pos - 1, Tokenizer.KEYWORD)
                end
            elseif is_api_word(word) then
                add(start, pos - 1, Tokenizer.API)
            elseif word:find("%.") then
                -- Dotted name that's not API — treat as default
                add(start, pos - 1, Tokenizer.DEFAULT)
            else
                -- Check if followed by "(" — it's a function call
                local after = line:sub(pos):match("^%s*%(")
                if after then
                    add(start, pos - 1, Tokenizer.FUNCNAME)
                else
                    add(start, pos - 1, Tokenizer.DEFAULT)
                end
            end

        -- Everything else (operators, punctuation)
        else
            pos = pos + 1
        end
    end

    return tokens, nil
end

return Tokenizer
