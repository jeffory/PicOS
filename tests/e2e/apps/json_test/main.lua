-- JSON Test — regression fixture for picocalc.json
--
-- Logs "JT: <name> PASS" / "JT: <name> FAIL <detail>" per case, then
-- "JT:DONE pass=N fail=M". A harness waits for JT:DONE and asserts fail=0.

local pc = picocalc
local json = pc.json

local pass, fail = 0, 0

local function ok(name, cond, detail)
    if cond then
        pass = pass + 1
        pc.sys.log("JT: " .. name .. " PASS")
    else
        fail = fail + 1
        pc.sys.log("JT: " .. name .. " FAIL " .. tostring(detail or ""))
    end
end

-- Deep structural compare, treating json.null as a value.
local function same(a, b)
    if type(a) ~= type(b) then return false end
    if type(a) ~= "table" then return a == b end
    for k, v in pairs(a) do
        if not same(v, b[k]) then return false end
    end
    for k in pairs(b) do
        if a[k] == nil then return false end
    end
    return true
end

local function roundtrip(name, value)
    local enc = json.encode(value)
    local dec, err = json.decode(enc)
    if dec == nil then
        ok(name, false, "decode failed: " .. tostring(err) .. " enc=" .. enc)
        return
    end
    ok(name, same(value, dec), "enc=" .. enc)
end

-- ── module presence ──────────────────────────────────────────────────────────

ok("module_exists", type(json) == "table", type(json))
ok("has_encode", type(json.encode) == "function")
ok("has_decode", type(json.decode) == "function")
ok("has_null", json.null ~= nil)

-- ── scalars ──────────────────────────────────────────────────────────────────

ok("enc_true",   json.encode(true) == "true",   json.encode(true))
ok("enc_false",  json.encode(false) == "false", json.encode(false))
ok("enc_int",    json.encode(42) == "42",       json.encode(42))
ok("enc_neg",    json.encode(-7) == "-7",       json.encode(-7))
ok("enc_string", json.encode("hi") == '"hi"',   json.encode("hi"))

-- Integers must stay integers: 1 must not come back as 1.0. This is the
-- LUA_32BITS integer/float distinction the encoder has to preserve.
local n = json.decode("42")
ok("dec_int_is_integer", math.type(n) == "integer", tostring(math.type(n)))
local f = json.decode("42.5")
ok("dec_float_is_float", math.type(f) == "float", tostring(math.type(f)))

-- ── nesting: the case game.save silently destroyed ───────────────────────────

roundtrip("rt_flat",   { a = 1, b = "two", c = true })
roundtrip("rt_nested", { outer = { inner = { deep = 5 } } })
roundtrip("rt_array",  { 1, 2, 3, 4, 5 })
roundtrip("rt_mixed",  { name = "grid", size = { w = 15, h = 15 },
                         clues = { { 1, 2 }, { 3 }, { 1, 1, 1 } } })

local deep = { lvl = 1, kid = { lvl = 2, kid = { lvl = 3, kid = { lvl = 4 } } } }
roundtrip("rt_deep4", deep)

-- Arrays of tables — the exact shape a puzzle library index uses.
roundtrip("rt_index", {
    v = 1,
    items = {
        { id = "p_a1", name = "CAT",  w = 10, h = 10 },
        { id = "p_b2", name = "SKULL", w = 15, h = 15 },
    },
})

-- ── strings and escapes ──────────────────────────────────────────────────────

roundtrip("rt_escapes", { s = "quote\" back\\slash\nnewline\ttab" })
ok("enc_ctrl_escaped",
   json.encode({ s = "\1" }):find("\\u0001") ~= nil,
   json.encode({ s = "\1" }))

local uni = json.decode('{"s":"\\u00e9"}')
ok("dec_unicode", uni ~= nil and uni.s == "\xc3\xa9",
   uni and string.byte(uni.s, 1) .. "," .. tostring(string.byte(uni.s, 2)))

local surro = json.decode('{"s":"\\ud83d\\ude00"}')   -- U+1F600, 4-byte UTF-8
ok("dec_surrogate_pair", surro ~= nil and #surro.s == 4,
   surro and #surro.s or "nil")

-- ── null handling ────────────────────────────────────────────────────────────

local withnull = json.decode('{"a":null,"b":2}')
ok("dec_null_preserves_key", withnull ~= nil and withnull.a ~= nil,
   "a=" .. tostring(withnull and withnull.a))
ok("dec_null_is_null", withnull ~= nil and json.isNull(withnull.a))
ok("dec_null_identity", withnull ~= nil and withnull.a == json.null)
ok("enc_null", json.encode({ a = json.null }) == '{"a":null}',
   json.encode({ a = json.null }))

-- ── documents larger than one buffer growth ──────────────────────────────────
--
-- Every case above fits in LUAL_BUFFERSIZE, which is why the encoder's original
-- luaL_Buffer misuse went unnoticed: the bug only bites once the output has to
-- grow. LUAL_BUFFERSIZE is 16 * sizeof(void*) * sizeof(lua_Number), which is
-- 512 bytes in the simulator and 256 on the RP2350 (LUA_32BITS makes
-- lua_Number a float), so these cases are deliberately kilobytes wide to force
-- several growths on both. Neurogram's records.json crossed 256 bytes at
-- roughly the seventh puzzle.

-- A per-key map, the shape a records/progress file uses.
local bigmap = {}
for i = 1, 40 do
    bigmap[("p_%08x"):format(i * 0x1234567)] =
        { solved = true, secs = 40 + i, moves = 90 + i, plays = 2 }
end
roundtrip("rt_big_map", { v = 1, records = bigmap })

-- Key count must survive, not just decodability: a truncated document can still
-- parse if the damage lands on a boundary.
local bigenc = json.encode({ v = 1, records = bigmap })
local bigdec = json.decode(bigenc)
local nkeys = 0
if bigdec and type(bigdec.records) == "table" then
    for _ in pairs(bigdec.records) do nkeys = nkeys + 1 end
end
ok("big_map_keeps_every_key", nkeys == 40, "got " .. nkeys .. " of 40")

-- One long string: growth happens mid-value rather than between keys.
local long = string.rep("cyberpunk-", 500)          -- 5000 bytes
local ls = json.decode(json.encode({ s = long }))
ok("rt_long_string", ls ~= nil and ls.s == long,
   ls and ("len=" .. #tostring(ls.s)) or "decode failed")

-- A long array, and nested tables deep inside a large document.
local arr = {}
for i = 1, 400 do arr[i] = i * 7 end
local ad = json.decode(json.encode(arr))
ok("rt_big_array", ad ~= nil and #ad == 400 and ad[400] == 2800,
   ad and ("n=" .. #ad .. " last=" .. tostring(ad[400])) or "decode failed")

local nested = {}
for i = 1, 30 do
    nested[i] = { id = i, name = ("row-%03d"):format(i),
                  clues = { { 1, 2, 3 }, { 4, 5 }, { 6 } },
                  meta = { tag = "abcdefghij", flag = (i % 2 == 0) } }
end
roundtrip("rt_big_nested", { v = 1, rows = nested })

-- Pretty-printing multiplies the output size, so indent must grow safely too.
local pbig = json.encode({ v = 1, records = bigmap }, { indent = 2 })
local pdec = json.decode(pbig)
ok("rt_big_indented", pdec ~= nil and type(pdec.records) == "table",
   "len=" .. #pbig)

-- ── malformed input returns nil,err rather than raising ───────────────────────

local bad = {
    "{",  "[",  '{"a"}', '{"a":}', "[1,]", '{"a":1,}',
    "tru", "nul", '"unterminated', "{'single':1}", "", "  ",
    '{"a":1} trailing',
}
local all_soft = true
local firstbad = nil
for _, s in ipairs(bad) do
    local okc, v, e = pcall(json.decode, s)
    if not okc then
        all_soft = false; firstbad = "raised on: " .. s
        break
    end
    if v ~= nil then
        all_soft = false; firstbad = "accepted invalid: " .. s
        break
    end
    if type(e) ~= "string" then
        all_soft = false; firstbad = "no errmsg for: " .. s
        break
    end
end
ok("malformed_soft_fail", all_soft, firstbad)

-- ── depth cap must error, not hard-fault ─────────────────────────────────────

local bomb = string.rep("[", 200) .. string.rep("]", 200)
local okc, v = pcall(json.decode, bomb)
ok("deep_input_no_crash", okc and v == nil, "pcall=" .. tostring(okc))

-- A cyclic table must raise a clean Lua error, not blow the C stack.
local cyc = {}; cyc.self = cyc
local okc2, err2 = pcall(json.encode, cyc)
ok("cycle_errors_cleanly", not okc2 and type(err2) == "string", tostring(err2))

-- ── empty table ──────────────────────────────────────────────────────────────

ok("enc_empty_table", json.encode({}) == "{}", json.encode({}))

-- ── indent option ────────────────────────────────────────────────────────────

local pretty = json.encode({ a = 1 }, { indent = 2 })
ok("indent_has_newline", pretty:find("\n") ~= nil, pretty)
local reparsed = json.decode(pretty)
ok("indent_still_parses", reparsed ~= nil and reparsed.a == 1, pretty)

-- ── game.save nested fix (the behaviour bug this module exists to fix) ───────

if pc.game and pc.game.save then
    pc.game.save.set("json_test_probe", {
        flat = 7,
        nested = { deep = "kept" },
        list = { 1, 2, 3 },
    })
    local back = pc.game.save.get("json_test_probe")
    ok("gamesave_nested_survives",
       back ~= nil and type(back.nested) == "table" and back.nested.deep == "kept",
       back and type(back.nested) == "table"
           and tostring(back.nested.deep)
           or "nested=" .. tostring(back and back.nested))
    ok("gamesave_flat_still_works", back ~= nil and back.flat == 7,
       back and tostring(back.flat))
    ok("gamesave_list_survives",
       back ~= nil and type(back.list) == "table" and back.list[2] == 2,
       back and type(back.list) or "nil")
    -- Backward compatibility: save files written by the OLD flat encoder must
    -- still load. /saves/legacy_flat.json holds the exact shape the shipped
    -- guineapig save uses ({"high_score":695}).
    if pc.game.save.exists("legacy_flat") then
        local old = pc.game.save.get("legacy_flat")
        ok("gamesave_legacy_flat_loads",
           old ~= nil and old.high_score == 695,
           old and tostring(old.high_score) or "nil")
    else
        pc.sys.log("JT: gamesave_legacy_flat_loads SKIP (no fixture file)")
    end
else
    pc.sys.log("JT: gamesave_skipped (pc.game.save absent)")
end

pc.sys.log(("JT:DONE pass=%d fail=%d"):format(pass, fail))

-- Outlive the harness sampling window; the launcher repaints the moment this
-- script returns.
pc.sys.sleep(3000)
