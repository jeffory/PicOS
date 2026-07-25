// picocalc.json — JSON encode/decode for Lua apps.
//
// Added because there was no JSON anywhere in the Lua SDK: every app that
// needed structured persistence hand-rolled a serializer (apps/calculator,
// apps/picoforge, apps/ssh, ...), the launcher carries its own minimal parser,
// and game.save's flat encoder silently turned nested tables into `null`.
//
// Design notes:
//
//  * Both directions are ITERATIVE where recursion depth would track input
//    size, and depth-capped at JSON_MAX_DEPTH otherwise. Lua is built with
//    LUAI_MAXSTACK=500 here, and the C stack is 4KB on core 0, so a deeply
//    nested document must produce a clean Lua error rather than a hard fault.
//
//  * decode returns `nil, errmsg` (never raises) so callers can handle bad
//    input from the network or a corrupted SD file without pcall. encode DOES
//    raise, because its failure modes are programmer errors (cycle, unsupported
//    type) rather than untrusted data.
//
//  * Lua has one table type for both arrays and objects. We treat a table as an
//    array iff it has at least one element and its integer keys are exactly
//    1..n with no other keys. An empty table encodes as `{}`.
//
//  * json.null is a unique sentinel userdata. Storing a real nil in a table
//    erases the key, so round-tripping `{"a":null}` needs a stand-in.

#include "lua_bridge_internal.h"
#include <math.h>

#define JSON_MAX_DEPTH 32

// ── json.null sentinel ───────────────────────────────────────────────────────

#define JSON_NULL_MT "picocalc.json.null"

static int json_null_tostring(lua_State *L) {
  lua_pushstring(L, "json.null");
  return 1;
}

// Pushes the singleton null sentinel onto the stack.
static void json_push_null(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, JSON_NULL_MT);
}

static bool json_is_null(lua_State *L, int idx) {
  if (lua_type(L, idx) != LUA_TUSERDATA) return false;
  idx = lua_absindex(L, idx);
  json_push_null(L);
  bool same = lua_rawequal(L, idx, -1);
  lua_pop(L, 1);
  return same;
}

// ── Encoder ──────────────────────────────────────────────────────────────────

typedef struct {
  luaL_Buffer buf;
  int indent;       // 0 = compact
} json_enc_t;

static void enc_value(lua_State *L, json_enc_t *e, int idx, int depth);

static void enc_newline_indent(json_enc_t *e, int depth) {
  if (e->indent <= 0) return;
  luaL_addchar(&e->buf, '\n');
  int n = e->indent * depth;
  for (int i = 0; i < n; i++) luaL_addchar(&e->buf, ' ');
}

static void enc_string(json_enc_t *e, const char *s, size_t len) {
  luaL_addchar(&e->buf, '"');
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  luaL_addstring(&e->buf, "\\\""); break;
      case '\\': luaL_addstring(&e->buf, "\\\\"); break;
      case '\b': luaL_addstring(&e->buf, "\\b");  break;
      case '\f': luaL_addstring(&e->buf, "\\f");  break;
      case '\n': luaL_addstring(&e->buf, "\\n");  break;
      case '\r': luaL_addstring(&e->buf, "\\r");  break;
      case '\t': luaL_addstring(&e->buf, "\\t");  break;
      default:
        if (c < 0x20) {
          // Control characters must be escaped; \u00XX is the portable form.
          char tmp[7];
          snprintf(tmp, sizeof(tmp), "\\u%04x", c);
          luaL_addstring(&e->buf, tmp);
        } else {
          // Bytes >= 0x20 pass through verbatim, which keeps valid UTF-8
          // intact without needing to decode it.
          luaL_addchar(&e->buf, (char)c);
        }
        break;
    }
  }
  luaL_addchar(&e->buf, '"');
}

static void enc_number(lua_State *L, json_enc_t *e, int idx) {
  char tmp[40];
  if (lua_isinteger(L, idx)) {
    lua_Integer v = lua_tointeger(L, idx);
    snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
  } else {
    lua_Number v = lua_tonumber(L, idx);
    // JSON has no way to spell NaN or Infinity. Emitting them would produce a
    // document no parser accepts, so fail loudly instead.
    if (isnan(v) || isinf(v))
      luaL_error(L, "json.encode: cannot encode %s", isnan(v) ? "NaN" : "Infinity");
    // %.14g round-trips a double without trailing float noise.
    snprintf(tmp, sizeof(tmp), "%.14g", (double)v);
  }
  luaL_addstring(&e->buf, tmp);
}

// Returns the array length n if the table at idx is a clean 1..n sequence,
// or -1 if it should be encoded as an object.
static lua_Integer enc_array_len(lua_State *L, int idx) {
  lua_Integer n = 0;
  lua_Integer count = 0;

  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    count++;
    if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
      lua_Integer k = lua_tointeger(L, -2);
      if (k >= 1) {
        if (k > n) n = k;
      } else {
        lua_pop(L, 2);
        return -1;  // zero or negative index -> object
      }
    } else {
      lua_pop(L, 2);
      return -1;    // non-integer key -> object
    }
    lua_pop(L, 1);
  }

  if (count == 0) return 0;      // empty table -> []  (see note below)
  if (n != count) return -1;     // sparse -> object, so no key is lost
  return n;
}

static void enc_table(lua_State *L, json_enc_t *e, int idx, int depth) {
  if (depth >= JSON_MAX_DEPTH)
    luaL_error(L, "json.encode: nesting deeper than %d (cycle?)", JSON_MAX_DEPTH);

  lua_Integer alen = enc_array_len(L, idx);

  if (alen >= 0) {
    // An empty Lua table is ambiguous; {} is the safer default because a table
    // used as a map is far more common in config/save data than an empty list,
    // and [] would decode back as a table that then re-encodes as {}.
    if (alen == 0) { luaL_addstring(&e->buf, "{}"); return; }

    luaL_addchar(&e->buf, '[');
    for (lua_Integer i = 1; i <= alen; i++) {
      if (i > 1) luaL_addchar(&e->buf, ',');
      enc_newline_indent(e, depth + 1);
      lua_rawgeti(L, idx, i);
      enc_value(L, e, lua_gettop(L), depth + 1);
      lua_pop(L, 1);
    }
    enc_newline_indent(e, depth);
    luaL_addchar(&e->buf, ']');
    return;
  }

  luaL_addchar(&e->buf, '{');
  bool first = true;
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    int ktype = lua_type(L, -2);
    if (ktype != LUA_TSTRING && ktype != LUA_TNUMBER) {
      // Skip keys JSON cannot name rather than emitting something invalid.
      lua_pop(L, 1);
      continue;
    }

    if (!first) luaL_addchar(&e->buf, ',');
    first = false;
    enc_newline_indent(e, depth + 1);

    if (ktype == LUA_TSTRING) {
      size_t klen;
      const char *k = lua_tolstring(L, -2, &klen);
      enc_string(e, k, klen);
    } else {
      // Numeric key: JSON object keys must be strings, so quote it.
      // lua_tolstring on the key would convert it in place and break lua_next,
      // so format from a copy.
      lua_pushvalue(L, -2);
      size_t klen;
      const char *k = lua_tolstring(L, -1, &klen);
      enc_string(e, k, klen);
      lua_pop(L, 1);
    }

    luaL_addchar(&e->buf, ':');
    if (e->indent > 0) luaL_addchar(&e->buf, ' ');
    enc_value(L, e, lua_gettop(L), depth + 1);
    lua_pop(L, 1);
  }
  enc_newline_indent(e, depth);
  luaL_addchar(&e->buf, '}');
}

static void enc_value(lua_State *L, json_enc_t *e, int idx, int depth) {
  idx = lua_absindex(L, idx);

  if (json_is_null(L, idx)) { luaL_addstring(&e->buf, "null"); return; }

  switch (lua_type(L, idx)) {
    case LUA_TNIL:
      luaL_addstring(&e->buf, "null");
      break;
    case LUA_TBOOLEAN:
      luaL_addstring(&e->buf, lua_toboolean(L, idx) ? "true" : "false");
      break;
    case LUA_TNUMBER:
      enc_number(L, e, idx);
      break;
    case LUA_TSTRING: {
      size_t len;
      const char *s = lua_tolstring(L, idx, &len);
      enc_string(e, s, len);
      break;
    }
    case LUA_TTABLE:
      enc_table(L, e, idx, depth);
      break;
    default:
      luaL_error(L, "json.encode: cannot encode a %s value",
                 luaL_typename(L, idx));
  }
}

// json.encode(value [, opts])   opts = { indent = n }
static int l_json_encode(lua_State *L) {
  json_enc_t e;
  e.indent = 0;

  if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
    lua_getfield(L, 2, "indent");
    if (!lua_isnil(L, -1)) {
      int n = (int)luaL_checkinteger(L, -1);
      if (n < 0) n = 0;
      if (n > 8) n = 8;
      e.indent = n;
    }
    lua_pop(L, 1);
  }

  luaL_buffinit(L, &e.buf);
  enc_value(L, &e, 1, 0);
  luaL_pushresult(&e.buf);
  return 1;
}

// ── Decoder ──────────────────────────────────────────────────────────────────

typedef struct {
  const char *s;
  size_t len;
  size_t pos;
  const char *err;
} json_dec_t;

static bool dec_value(lua_State *L, json_dec_t *d, int depth);

static void dec_skip_ws(json_dec_t *d) {
  while (d->pos < d->len) {
    char c = d->s[d->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') d->pos++;
    else break;
  }
}

static bool dec_fail(json_dec_t *d, const char *msg) {
  if (!d->err) d->err = msg;
  return false;
}

static bool dec_literal(json_dec_t *d, const char *lit) {
  size_t n = strlen(lit);
  if (d->pos + n > d->len) return false;
  if (memcmp(d->s + d->pos, lit, n) != 0) return false;
  d->pos += n;
  return true;
}

// Appends a code point to buf as UTF-8.
static void dec_utf8(luaL_Buffer *buf, unsigned long cp) {
  if (cp < 0x80) {
    luaL_addchar(buf, (char)cp);
  } else if (cp < 0x800) {
    luaL_addchar(buf, (char)(0xC0 | (cp >> 6)));
    luaL_addchar(buf, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    luaL_addchar(buf, (char)(0xE0 | (cp >> 12)));
    luaL_addchar(buf, (char)(0x80 | ((cp >> 6) & 0x3F)));
    luaL_addchar(buf, (char)(0x80 | (cp & 0x3F)));
  } else {
    luaL_addchar(buf, (char)(0xF0 | (cp >> 18)));
    luaL_addchar(buf, (char)(0x80 | ((cp >> 12) & 0x3F)));
    luaL_addchar(buf, (char)(0x80 | ((cp >> 6) & 0x3F)));
    luaL_addchar(buf, (char)(0x80 | (cp & 0x3F)));
  }
}

static int dec_hex4(json_dec_t *d, unsigned long *out) {
  if (d->pos + 4 > d->len) return 0;
  unsigned long v = 0;
  for (int i = 0; i < 4; i++) {
    char c = d->s[d->pos + i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
    else return 0;
  }
  d->pos += 4;
  *out = v;
  return 1;
}

static bool dec_string(lua_State *L, json_dec_t *d) {
  if (d->pos >= d->len || d->s[d->pos] != '"')
    return dec_fail(d, "expected string");
  d->pos++;

  luaL_Buffer b;
  luaL_buffinit(L, &b);

  while (d->pos < d->len) {
    unsigned char c = (unsigned char)d->s[d->pos++];

    if (c == '"') { luaL_pushresult(&b); return true; }

    if (c != '\\') {
      if (c < 0x20) { luaL_pushresult(&b); lua_pop(L, 1);
                      return dec_fail(d, "control character in string"); }
      luaL_addchar(&b, (char)c);
      continue;
    }

    if (d->pos >= d->len) { luaL_pushresult(&b); lua_pop(L, 1);
                            return dec_fail(d, "truncated escape"); }

    char esc = d->s[d->pos++];
    switch (esc) {
      case '"':  luaL_addchar(&b, '"');  break;
      case '\\': luaL_addchar(&b, '\\'); break;
      case '/':  luaL_addchar(&b, '/');  break;
      case 'b':  luaL_addchar(&b, '\b'); break;
      case 'f':  luaL_addchar(&b, '\f'); break;
      case 'n':  luaL_addchar(&b, '\n'); break;
      case 'r':  luaL_addchar(&b, '\r'); break;
      case 't':  luaL_addchar(&b, '\t'); break;
      case 'u': {
        unsigned long cp;
        if (!dec_hex4(d, &cp)) { luaL_pushresult(&b); lua_pop(L, 1);
                                 return dec_fail(d, "bad \\u escape"); }
        // Surrogate pair: high surrogate must be followed by \uDC00-\uDFFF.
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (d->pos + 6 <= d->len && d->s[d->pos] == '\\' &&
              d->s[d->pos + 1] == 'u') {
            size_t save = d->pos;
            d->pos += 2;
            unsigned long lo;
            if (dec_hex4(d, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              d->pos = save;  // lone high surrogate; emit as-is
            }
          }
        }
        dec_utf8(&b, cp);
        break;
      }
      default:
        luaL_pushresult(&b);
        lua_pop(L, 1);
        return dec_fail(d, "unknown escape");
    }
  }

  luaL_pushresult(&b);
  lua_pop(L, 1);
  return dec_fail(d, "unterminated string");
}

static bool dec_number(lua_State *L, json_dec_t *d) {
  size_t start = d->pos;
  bool is_float = false;

  if (d->pos < d->len && (d->s[d->pos] == '-' || d->s[d->pos] == '+')) d->pos++;
  while (d->pos < d->len) {
    char c = d->s[d->pos];
    if (c >= '0' && c <= '9') { d->pos++; }
    else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
      is_float = true; d->pos++;
    } else break;
  }

  if (d->pos == start) return dec_fail(d, "expected number");

  // strtod/strtoll need a NUL-terminated copy; numbers are short so a small
  // stack buffer is enough, and an over-long run is malformed anyway.
  char tmp[64];
  size_t n = d->pos - start;
  if (n >= sizeof(tmp)) return dec_fail(d, "number too long");
  memcpy(tmp, d->s + start, n);
  tmp[n] = '\0';

  char *end = NULL;
  if (!is_float) {
    long long v = strtoll(tmp, &end, 10);
    if (end && *end == '\0') {
      // Keep integers as Lua integers so round-tripping does not turn 1 into
      // 1.0. LUA_32BITS=1 here, so lua_Integer is 32-bit: fall back to a float
      // if the value does not fit.
      if (v >= (long long)LUA_MININTEGER && v <= (long long)LUA_MAXINTEGER) {
        lua_pushinteger(L, (lua_Integer)v);
        return true;
      }
      lua_pushnumber(L, (lua_Number)v);
      return true;
    }
  }

  double dv = strtod(tmp, &end);
  if (!end || *end != '\0') return dec_fail(d, "malformed number");
  lua_pushnumber(L, (lua_Number)dv);
  return true;
}

static bool dec_array(lua_State *L, json_dec_t *d, int depth) {
  d->pos++;  // consume '['
  lua_newtable(L);
  lua_Integer i = 1;

  dec_skip_ws(d);
  if (d->pos < d->len && d->s[d->pos] == ']') { d->pos++; return true; }

  for (;;) {
    dec_skip_ws(d);
    if (!dec_value(L, d, depth + 1)) { lua_pop(L, 1); return false; }
    lua_rawseti(L, -2, i++);

    dec_skip_ws(d);
    if (d->pos >= d->len) { lua_pop(L, 1); return dec_fail(d, "unterminated array"); }
    if (d->s[d->pos] == ',') { d->pos++; continue; }
    if (d->s[d->pos] == ']') { d->pos++; return true; }
    lua_pop(L, 1);
    return dec_fail(d, "expected ',' or ']' in array");
  }
}

static bool dec_object(lua_State *L, json_dec_t *d, int depth) {
  d->pos++;  // consume '{'
  lua_newtable(L);

  dec_skip_ws(d);
  if (d->pos < d->len && d->s[d->pos] == '}') { d->pos++; return true; }

  for (;;) {
    dec_skip_ws(d);
    if (!dec_string(L, d)) { lua_pop(L, 1); return false; }

    dec_skip_ws(d);
    if (d->pos >= d->len || d->s[d->pos] != ':') {
      lua_pop(L, 2);
      return dec_fail(d, "expected ':' after object key");
    }
    d->pos++;

    dec_skip_ws(d);
    if (!dec_value(L, d, depth + 1)) { lua_pop(L, 2); return false; }
    lua_rawset(L, -3);

    dec_skip_ws(d);
    if (d->pos >= d->len) { lua_pop(L, 1); return dec_fail(d, "unterminated object"); }
    if (d->s[d->pos] == ',') { d->pos++; continue; }
    if (d->s[d->pos] == '}') { d->pos++; return true; }
    lua_pop(L, 1);
    return dec_fail(d, "expected ',' or '}' in object");
  }
}

static bool dec_value(lua_State *L, json_dec_t *d, int depth) {
  if (depth >= JSON_MAX_DEPTH) return dec_fail(d, "nesting too deep");

  dec_skip_ws(d);
  if (d->pos >= d->len) return dec_fail(d, "unexpected end of input");

  char c = d->s[d->pos];
  switch (c) {
    case '{': return dec_object(L, d, depth);
    case '[': return dec_array(L, d, depth);
    case '"': return dec_string(L, d);
    case 't':
      if (dec_literal(d, "true"))  { lua_pushboolean(L, 1); return true; }
      return dec_fail(d, "expected 'true'");
    case 'f':
      if (dec_literal(d, "false")) { lua_pushboolean(L, 0); return true; }
      return dec_fail(d, "expected 'false'");
    case 'n':
      if (dec_literal(d, "null"))  { json_push_null(L); return true; }
      return dec_fail(d, "expected 'null'");
    default:
      return dec_number(L, d);
  }
}

// json.decode(str) -> value, or nil, errmsg
static int l_json_decode(lua_State *L) {
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);

  json_dec_t d = { .s = s, .len = len, .pos = 0, .err = NULL };

  if (!dec_value(L, &d, 0)) {
    lua_pushnil(L);
    lua_pushfstring(L, "json.decode: %s at byte %d",
                    d.err ? d.err : "parse error", (int)d.pos + 1);
    return 2;
  }

  dec_skip_ws(&d);
  if (d.pos != d.len) {
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_pushfstring(L, "json.decode: trailing content at byte %d",
                    (int)d.pos + 1);
    return 2;
  }

  return 1;
}

// json.isNull(v) -> bool.  Saves apps from importing the sentinel to compare.
static int l_json_isNull(lua_State *L) {
  lua_pushboolean(L, json_is_null(L, 1));
  return 1;
}

// ── C entry points for other bridge modules ──────────────────────────────────
//
// game.save is built on these so there is exactly one JSON implementation in
// the firmware rather than a second hand-rolled one that drifts.

// Encodes the value at `idx` and pushes the resulting string.
// Raises a Lua error on cycles / unsupported types, like json.encode.
void lua_json_encode_push(lua_State *L, int idx, int indent) {
  json_enc_t e;
  e.indent = (indent < 0) ? 0 : (indent > 8 ? 8 : indent);
  luaL_buffinit(L, &e.buf);
  enc_value(L, &e, idx, 0);
  luaL_pushresult(&e.buf);
}

// Decodes `s` and pushes the value, returning true. On failure pushes nothing,
// returns false, and sets *err to a static description.
bool lua_json_decode_push(lua_State *L, const char *s, size_t len,
                          const char **err) {
  json_dec_t d = { .s = s, .len = len, .pos = 0, .err = NULL };
  if (!dec_value(L, &d, 0)) {
    if (err) *err = d.err ? d.err : "parse error";
    return false;
  }
  dec_skip_ws(&d);
  if (d.pos != d.len) {
    lua_pop(L, 1);
    if (err) *err = "trailing content";
    return false;
  }
  return true;
}

// ── Registration ─────────────────────────────────────────────────────────────

static const luaL_Reg l_json_lib[] = {
    {"encode", l_json_encode},
    {"decode", l_json_decode},
    {"isNull", l_json_isNull},
    {NULL, NULL}};

void lua_bridge_json_init(lua_State *L) {
  // Create the null sentinel once and stash it in the registry so every
  // decode returns the identical value and `v == picocalc.json.null` works.
  if (lua_getfield(L, LUA_REGISTRYINDEX, JSON_NULL_MT) == LUA_TNIL) {
    lua_pop(L, 1);
    lua_newuserdatauv(L, 1, 0);
    lua_newtable(L);
    lua_pushcfunction(L, json_null_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushstring(L, JSON_NULL_MT);
    lua_setfield(L, -2, "__name");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, JSON_NULL_MT);
  }
  lua_pop(L, 1);

  register_subtable(L, "json", l_json_lib);

  lua_getfield(L, -1, "json");
  json_push_null(L);
  lua_setfield(L, -2, "null");
  lua_pop(L, 1);
}
