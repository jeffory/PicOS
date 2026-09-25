-- sharecode.lua — compact text encoding of a puzzle, for offline sharing and as
-- the canonical wire format for the share API. Pure logic, no picocalc.*.
--
-- Alphabet: Crockford Base32 (0-9 A-Z minus I L O U), 5 bits per character.
--
-- Base32 over base64url is a deliberate UX call, not an oversight. Base64url
-- needs mixed case, which means reaching for shift across ~100 keystrokes on the
-- PicoCalc's thumb keyboard. Crockford is case-insensitive, drops the four
-- characters people misread (I/L/O/U vs 1/1/0/V), and costs about 18% length —
-- which every supported size can afford:
--
--   size    grid bits   payload chars   total   ui.textInput budget (127)
--   5x5        25             5           10          fits
--   10x10     100            20           25          fits
--   15x15     225            45           50          fits
--   20x20     400            80           85          fits, 42 spare
--
-- So no RLE and no compression: at typical nonogram densities (~45-55% filled)
-- run-length coding would usually make the payload longer, not shorter.
--
-- Format:  N1<size><payload><ck0><ck1>[:<title>]
--   N        magic
--   1        format version
--   <size>   one char indexing SIZES below; the escape entry carries explicit
--            dimensions in the two following chars
--   <ck>     two chars = 10-bit checksum over version+size+payload
--
-- The title separator is ':' and NOT '-', because '-' is the grouping character
-- S.group() inserts for readability. Using the same character for both would
-- make a grouped code parse as body + title.

local S = {}

local ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"   -- 32 chars, no I L O U

local VALUE = {}
for i = 1, #ALPHABET do
    VALUE[ALPHABET:sub(i, i)] = i - 1
end
-- Crockford's documented confusable folding, so a mistyped code still resolves.
VALUE["I"], VALUE["L"] = 1, 1
VALUE["O"] = 0
VALUE["U"] = VALUE["V"]

-- Index -> {w, h}. Order is frozen: appending is fine, reordering breaks every
-- code already in the wild.
local SIZES = {
    { 5,  5 }, { 10, 10 }, { 15, 15 }, { 20, 20 },
    { 10, 15 }, { 15, 10 }, { 12, 12 }, { 25, 25 },
}
local ESCAPE_INDEX = 31   -- explicit dimensions follow in the next two chars

S.SIZES = SIZES

local function sizeIndex(w, h)
    for i = 1, #SIZES do
        if SIZES[i][1] == w and SIZES[i][2] == h then return i - 1 end
    end
    return nil
end

-- ── Bit packing ──────────────────────────────────────────────────────────────

-- Packs `count` values of `bits` each, MSB-first, into base32 characters.
function S.packBits(get, count, bits)
    local out, acc, accBits = {}, 0, 0
    for i = 1, count do
        acc = (acc << bits) | (get(i) & ((1 << bits) - 1))
        accBits = accBits + bits
        while accBits >= 5 do
            local shift = accBits - 5
            local v = (acc >> shift) & 0x1F
            out[#out + 1] = ALPHABET:sub(v + 1, v + 1)
            acc = acc & ((1 << shift) - 1)
            accBits = shift
        end
    end
    if accBits > 0 then
        local v = (acc << (5 - accBits)) & 0x1F
        out[#out + 1] = ALPHABET:sub(v + 1, v + 1)
    end
    return table.concat(out)
end

-- Inverse of packBits. Returns an array of `count` values, or nil on a bad char.
function S.unpackBits(str, count, bits)
    local out, acc, accBits, pos = {}, 0, 0, 1
    for i = 1, count do
        while accBits < bits do
            if pos > #str then return nil, "code too short" end
            local ch = str:sub(pos, pos)
            local v = VALUE[ch]
            if v == nil then return nil, "bad character '" .. ch .. "'" end
            acc = (acc << 5) | v
            accBits = accBits + 5
            pos = pos + 1
        end
        local shift = accBits - bits
        out[i] = (acc >> shift) & ((1 << bits) - 1)
        acc = acc & ((1 << shift) - 1)
        accBits = shift
    end
    return out
end

-- ── Checksum ─────────────────────────────────────────────────────────────────

-- 10-bit FNV-1a fold. Catches transcription slips; it is not a security
-- measure, and the format does not pretend otherwise.
local function checksum10(s)
    local h = 0x811C
    for i = 1, #s do
        h = h ~ s:byte(i)
        h = (h * 0x0193) & 0xFFFF
    end
    return ((h >> 6) ~ h) & 0x3FF
end

local function ckChars(payload)
    local ck = checksum10(payload)
    local hi = (ck >> 5) & 0x1F
    local lo = ck & 0x1F
    return ALPHABET:sub(hi + 1, hi + 1) .. ALPHABET:sub(lo + 1, lo + 1)
end

-- ── Encode / decode ──────────────────────────────────────────────────────────

-- encode(puzzle) -> code string
-- puzzle = { w, h, solution = {[r]={0|1,...}}, name = "..." }
function S.encode(puzzle)
    local w, h = puzzle.w, puzzle.h
    local sol = puzzle.solution

    local head
    local idx = sizeIndex(w, h)
    if idx then
        head = ALPHABET:sub(idx + 1, idx + 1)
    else
        -- Escape form: dimensions as two 5-bit chars (w-1, h-1), so up to 32x32.
        if w < 1 or w > 32 or h < 1 or h > 32 then
            return nil, "size out of range"
        end
        head = ALPHABET:sub(ESCAPE_INDEX + 1, ESCAPE_INDEX + 1)
                .. ALPHABET:sub(w, w) .. ALPHABET:sub(h, h)
    end

    local n = w * h
    local payload = S.packBits(function(i)
        local r = ((i - 1) // w) + 1
        local c = ((i - 1) % w) + 1
        return (sol[r][c] == 1) and 1 or 0
    end, n, 1)

    local body = "1" .. head .. payload
    local code = "N" .. body .. ckChars(body)

    if puzzle.name and #puzzle.name > 0 then
        code = code .. ":" .. puzzle.name
    end
    return code
end

-- Strips formatting so a user can type the code however they like: any
-- character outside the alphabet is dropped and letters are upper-cased.
local function normalizeBody(s)
    return (s:upper():gsub("[^0-9A-Z]", ""))
end

-- decode(code) -> puzzle, or nil, err
function S.decode(code)
    if type(code) ~= "string" then return nil, "not a string" end

    -- Everything after the first ':' is the title, taken verbatim — so a dash,
    -- space or further colon inside a title is harmless.
    local name = nil
    local body = code
    local sep = code:find(":", 1, true)
    if sep then
        name = code:sub(sep + 1)
        body = code:sub(1, sep - 1)
    end

    -- Grouping dashes and stray whitespace are stripped here.
    body = normalizeBody(body)

    if #body < 6 then return nil, "code too short" end
    if body:sub(1, 1) ~= "N" then return nil, "not a Neurogram code" end
    if body:sub(2, 2) ~= "1" then
        return nil, "unsupported version '" .. body:sub(2, 2) .. "'"
    end

    local ck = body:sub(-2)
    local signed = body:sub(2, #body - 2)   -- version + size + payload
    if ckChars(signed) ~= ck then
        return nil, "checksum mismatch — check for a typo"
    end

    local sizeChar = signed:sub(2, 2)
    local sIdx = VALUE[sizeChar]
    if sIdx == nil then return nil, "bad size character" end

    local w, h, payload
    if sIdx == ESCAPE_INDEX then
        if #signed < 4 then return nil, "truncated escape header" end
        local wv = VALUE[signed:sub(3, 3)]
        local hv = VALUE[signed:sub(4, 4)]
        if wv == nil or hv == nil then return nil, "bad escape dimensions" end
        w, h = wv + 1, hv + 1
        payload = signed:sub(5)
    else
        local sz = SIZES[sIdx + 1]
        if not sz then return nil, "unknown size index " .. sIdx end
        w, h = sz[1], sz[2]
        payload = signed:sub(3)
    end

    local bits, err = S.unpackBits(payload, w * h, 1)
    if not bits then return nil, err end

    local solution = {}
    for r = 1, h do
        solution[r] = {}
        for c = 1, w do
            solution[r][c] = bits[(r - 1) * w + c]
        end
    end

    return { w = w, h = h, solution = solution, name = name }
end

-- ui.textInput caps entry at 127 characters, so a code longer than that can
-- still be shared by file or over the API but cannot be typed in by hand.
-- The four sizes the game offers (5, 10, 15, 20 square) all fit comfortably;
-- 25x25 and the larger escape-form sizes do not.
S.TEXTINPUT_MAX = 127

function S.fitsTextInput(code)
    return #code <= S.TEXTINPUT_MAX
end

-- Inserts a separator every `n` characters for display. The decoder strips
-- these, so a grouped code and a raw one are interchangeable.
function S.group(code, n)
    n = n or 10
    local sep = code:find(":", 1, true)
    local body = sep and code:sub(1, sep - 1) or code
    local tail = sep and code:sub(sep) or ""

    local parts = {}
    for i = 1, #body, n do parts[#parts + 1] = body:sub(i, i + n - 1) end
    return table.concat(parts, "-") .. tail
end

return S
