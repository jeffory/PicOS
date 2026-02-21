#include "lua_bridge_internal.h"

// ── picocalc.wifi.* ──────────────────────────────────────────────────────────

static int l_wifi_isAvailable(lua_State *L) {
  lua_pushboolean(L, wifi_is_available());
  return 1;
}

static int l_wifi_connect(lua_State *L) {
  const char *ssid = luaL_checkstring(L, 1);
  const char *pass = luaL_optstring(L, 2, "");
  wifi_connect(ssid, pass);
  return 0;
}

static int l_wifi_disconnect(lua_State *L) {
  (void)L;
  wifi_disconnect();
  return 0;
}

static int l_wifi_getStatus(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)wifi_get_status());
  return 1;
}

static int l_wifi_getIP(lua_State *L) {
  const char *ip = wifi_get_ip();
  if (ip)
    lua_pushstring(L, ip);
  else
    lua_pushnil(L);
  return 1;
}

static int l_wifi_getSSID(lua_State *L) {
  const char *ssid = wifi_get_ssid();
  if (ssid)
    lua_pushstring(L, ssid);
  else
    lua_pushnil(L);
  return 1;
}

static const luaL_Reg l_wifi_lib[] = {{"isAvailable", l_wifi_isAvailable},
                                      {"connect", l_wifi_connect},
                                      {"disconnect", l_wifi_disconnect},
                                      {"getStatus", l_wifi_getStatus},
                                      {"getIP", l_wifi_getIP},
                                      {"getSSID", l_wifi_getSSID},
                                      {NULL, NULL}};



// ── picocalc.network.* and picocalc.network.http.* ───────────────────────────
//
// picocalc.network.http.new() returns a Lua full-userdata object with method
// bindings via a metatable.  Callbacks are fired from menu_lua_hook (after
// wifi_poll() returns) — never from inside lwIP callbacks — so lua_pcall is
// always safe to call there.

#define HTTP_MT "picocalc.network.http" // metatable registry key

typedef struct {
  http_conn_t *conn; // NULL once closed/GC'd
  int cb_request;    // LUA_NOREF or registry ref
  int cb_headers;
  int cb_complete;
  int cb_closed;
} http_ud_t;

static void http_ud_unref_all(lua_State *L, http_ud_t *ud);

// ── HTTP callback dispatcher (called from menu_lua_hook)
// ──────────────────────

// Iterates the C connection pool, reads & clears pending flags, and fires the
// corresponding Lua callbacks via lua_pcall.  Safe because we are OUTSIDE of
// wifi_poll() / cyw43_arch_poll() when this runs.
void http_lua_fire_pending(lua_State *L) {
  for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
    http_conn_t *c = http_get_conn(i);
    if (!c || !c->lua_ud)
      continue;

    uint8_t pend = http_take_pending(c);
    if (!pend)
      continue;

    http_ud_t *ud = (http_ud_t *)c->lua_ud;

    // Fire in order: headers → data → complete → closed
    if ((pend & HTTP_CB_HEADERS) && ud->cb_headers != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_headers);
      if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        lua_pop(L, 1);
    }
    if ((pend & HTTP_CB_REQUEST) && ud->cb_request != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_request);
      if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        lua_pop(L, 1);
    }
    if ((pend & HTTP_CB_COMPLETE) && ud->cb_complete != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_complete);
      if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        lua_pop(L, 1);
    }
    if ((pend & (HTTP_CB_CLOSED | HTTP_CB_FAILED)) &&
        ud->cb_closed != LUA_NOREF) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_closed);
      if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        lua_pop(L, 1);
    }

    // If connection is closed or failed, unref all callbacks to break
    // potential closure cycles (callbacks capturing the 'conn' object).
    if (pend & (HTTP_CB_CLOSED | HTTP_CB_FAILED)) {
      http_ud_unref_all(L, ud);
    }
  }
}

// ── Helpers
// ───────────────────────────────────────────────────────────────────

static http_ud_t *check_http(lua_State *L, int idx) {
  return (http_ud_t *)luaL_checkudata(L, idx, HTTP_MT);
}

static http_ud_t *check_http_open(lua_State *L, int idx) {
  http_ud_t *ud = check_http(L, idx);
  if (!ud->conn)
    luaL_error(L, "http: connection is closed");
  return ud;
}

// Convert a Lua headers argument (string / array / kv-table) at stack index
// `idx` to a malloc'd "Key: Value\r\n..." string, or NULL if nil/absent.
static char *lua_headers_to_str(lua_State *L, int idx) {
  if (lua_isnoneornil(L, idx))
    return NULL;

  char *buf = umm_malloc(4096);
  if (!buf)
    return NULL;
  int n = 0;

  if (lua_isstring(L, idx)) {
    const char *s = lua_tostring(L, idx);
    n = snprintf(buf, 4096, "%s", s);
    if (n >= 2 && (buf[n - 2] != '\r' || buf[n - 1] != '\n'))
      n += snprintf(buf + n, 4096 - n, "\r\n");

  } else if (lua_istable(L, idx)) {
    int arr_len = (int)lua_rawlen(L, idx);
    if (arr_len > 0) {
      // Array of "Key: Value" strings
      for (int i = 1; i <= arr_len && n < 4080; i++) {
        lua_rawgeti(L, idx, i);
        const char *s = lua_tostring(L, -1);
        if (s) {
          n += snprintf(buf + n, 4096 - n, "%s", s);
          if (n >= 2 && (buf[n - 2] != '\r' || buf[n - 1] != '\n'))
            n += snprintf(buf + n, 4096 - n, "\r\n");
        }
        lua_pop(L, 1);
      }
    } else {
      // Key/value table
      lua_pushnil(L);
      while (lua_next(L, idx) != 0 && n < 4080) {
        const char *k = lua_tostring(L, -2);
        const char *v = lua_tostring(L, -1);
        if (k && v)
          n += snprintf(buf + n, 4096 - n, "%s: %s\r\n", k, v);
        lua_pop(L, 1);
      }
    }
  }

  if (n == 0) {
    umm_free(buf);
    return NULL;
  }
  return buf;
}

// Unref all callback registry entries on a userdata.
static void http_ud_unref_all(lua_State *L, http_ud_t *ud) {
  if (ud->cb_request != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_request);
    ud->cb_request = LUA_NOREF;
  }
  if (ud->cb_headers != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_headers);
    ud->cb_headers = LUA_NOREF;
  }
  if (ud->cb_complete != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_complete);
    ud->cb_complete = LUA_NOREF;
  }
  if (ud->cb_closed != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_closed);
    ud->cb_closed = LUA_NOREF;
  }
}

// ── Metatable methods
// ──────────────────────────────────────────────────────────

static int l_http_gc(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  if (ud->conn) {
    ud->conn->lua_ud = NULL;
    ud->conn->pending = 0;
    http_free(ud->conn);
    ud->conn = NULL;
  }
  http_ud_unref_all(L, ud);
  return 0;
}

// picocalc.network.http.new(server, [port], [usessl], [reason]) -> obj or nil,
// err
static int l_http_new(lua_State *L) {
  const char *server = luaL_checkstring(L, 1);
  bool use_ssl = lua_gettop(L) >= 3 && lua_toboolean(L, 3);

  lua_Integer port = luaL_optinteger(L, 2, use_ssl ? 443 : 80);

  http_conn_t *conn = http_alloc();
  if (!conn) {
    lua_pushnil(L);
    lua_pushstring(L, "HTTP connection pool full or out of memory");
    return 2;
  }

  strncpy(conn->server, server, HTTP_SERVER_MAX - 1);
  conn->port = (uint16_t)port;
  conn->use_ssl = use_ssl;

  http_ud_t *ud = (http_ud_t *)lua_newuserdata(L, sizeof(http_ud_t));
  ud->conn = conn;
  ud->cb_request = LUA_NOREF;
  ud->cb_headers = LUA_NOREF;
  ud->cb_complete = LUA_NOREF;
  ud->cb_closed = LUA_NOREF;
  conn->lua_ud = ud;

  luaL_getmetatable(L, HTTP_MT);
  lua_setmetatable(L, -2);
  return 1;
}

// conn:close()
static int l_http_close(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  if (ud->conn)
    http_close(ud->conn);
  return 0;
}

// conn:setKeepAlive(flag)
static int l_http_setKeepAlive(lua_State *L) {
  http_ud_t *ud = check_http_open(L, 1);
  ud->conn->keep_alive = lua_toboolean(L, 2);
  return 0;
}

// conn:setByteRange(from, to)
static int l_http_setByteRange(lua_State *L) {
  http_ud_t *ud = check_http_open(L, 1);
  ud->conn->range_from = (int32_t)luaL_checkinteger(L, 2);
  ud->conn->range_to = (int32_t)luaL_checkinteger(L, 3);
  return 0;
}

// conn:setConnectTimeout(seconds)
static int l_http_setConnectTimeout(lua_State *L) {
  http_ud_t *ud = check_http_open(L, 1);
  ud->conn->connect_timeout_ms = (uint32_t)(luaL_checknumber(L, 2) * 1000.0);
  return 0;
}

// conn:setReadTimeout(seconds)
static int l_http_setReadTimeout(lua_State *L) {
  http_ud_t *ud = check_http_open(L, 1);
  ud->conn->read_timeout_ms = (uint32_t)(luaL_checknumber(L, 2) * 1000.0);
  return 0;
}

// conn:setReadBufferSize(bytes)
static int l_http_setReadBufferSize(lua_State *L) {
  http_ud_t *ud = check_http_open(L, 1);
  http_set_recv_buf(ud->conn, (uint32_t)luaL_checkinteger(L, 2));
  return 0;
}

// Shared implementation for get / post.
// `has_body` = true  →  POST semantics: (self, path, [headers], data)
//                        if only one extra arg, treat it as data (no headers)
// `has_body` = false →  GET semantics:  (self, path, [headers])
static int do_request(lua_State *L, bool has_body) {
  http_ud_t *ud = check_http_open(L, 1);
  const char *path = luaL_checkstring(L, 2);

  char *hdrs = NULL;
  const char *body = NULL;
  size_t body_len = 0;
  int nargs = lua_gettop(L);

  if (has_body) {
    if (nargs == 3) {
      // (self, path, data) — single extra arg is body
      body = lua_tolstring(L, 3, &body_len);
    } else if (nargs >= 4) {
      // (self, path, headers, data)
      hdrs = lua_headers_to_str(L, 3);
      if (!lua_isnoneornil(L, 4))
        body = lua_tolstring(L, 4, &body_len);
    }
  } else {
    // GET: (self, path, [headers])
    if (nargs >= 3)
      hdrs = lua_headers_to_str(L, 3);
  }

  bool ok = has_body ? http_post(ud->conn, path, hdrs, body, body_len)
                     : http_get(ud->conn, path, hdrs);

  umm_free(hdrs);

  lua_pushboolean(L, ok);
  if (!ok) {
    lua_pushstring(L, ud->conn->err);
    return 2;
  }
  return 1;
}

static int l_http_get(lua_State *L) { return do_request(L, false); }
static int l_http_post(lua_State *L) { return do_request(L, true); }

// conn:getError() -> string or nil
static int l_http_getError(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  if (!ud->conn || ud->conn->err[0] == '\0') {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, ud->conn->err);
  return 1;
}

// conn:getProgress() -> bytes_received, total (-1 if unknown)
static int l_http_getProgress(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  lua_pushinteger(L, ud->conn ? (lua_Integer)ud->conn->body_received : 0);
  lua_pushinteger(L, ud->conn ? (lua_Integer)ud->conn->content_length : -1);
  return 2;
}

// conn:getBytesAvailable() -> n
static int l_http_getBytesAvailable(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  lua_pushinteger(L, (lua_Integer)http_bytes_available(ud->conn));
  return 1;
}

// conn:read([length]) -> string or nil
static int l_http_read(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  if (!ud->conn) {
    lua_pushnil(L);
    return 1;
  }

  uint32_t avail = http_bytes_available(ud->conn);
  if (avail == 0) {
    lua_pushnil(L);
    return 1;
  }

  uint32_t want = avail;
  if (!lua_isnoneornil(L, 2)) {
    lua_Integer req = luaL_checkinteger(L, 2);
    if (req > 0 && (uint32_t)req < want)
      want = (uint32_t)req;
  }
  if (want > 65536)
    want = 65536;

  uint8_t *tmp = umm_malloc(want);
  if (!tmp) {
    lua_pushnil(L);
    return 1;
  }

  uint32_t n = http_read(ud->conn, tmp, want);
  if (n > 0)
    lua_pushlstring(L, (char *)tmp, n);
  else
    lua_pushnil(L);
  umm_free(tmp);
  return 1;
}

// conn:getResponseStatus() -> integer or nil
static int l_http_getResponseStatus(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  if (!ud->conn || ud->conn->status_code == 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, ud->conn->status_code);
  return 1;
}

// conn:getResponseHeaders() -> table {key=value} or nil
static int l_http_getResponseHeaders(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  if (!ud->conn || !ud->conn->headers_done) {
    lua_pushnil(L);
    return 1;
  }

  http_conn_t *c = ud->conn;
  lua_newtable(L);
  for (int i = 0; i < c->hdr_count; i++) {
    lua_pushstring(L, c->hdr_keys[i]);
    lua_pushstring(L, c->hdr_vals[i]);
    lua_settable(L, -3);
  }
  return 1;
}

// Generic callback setter: conn:set*Callback(fn)
static int set_http_cb(lua_State *L, int *ref) {
  if (!lua_isnoneornil(L, 2))
    luaL_checktype(L, 2, LUA_TFUNCTION);
  if (*ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, *ref);
    *ref = LUA_NOREF;
  }
  if (!lua_isnoneornil(L, 2)) {
    lua_pushvalue(L, 2);
    *ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  return 0;
}

static int l_http_setRequestCallback(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  return set_http_cb(L, &ud->cb_request);
}
static int l_http_setHeadersReadCallback(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  return set_http_cb(L, &ud->cb_headers);
}
static int l_http_setRequestCompleteCallback(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  return set_http_cb(L, &ud->cb_complete);
}
static int l_http_setConnectionClosedCallback(lua_State *L) {
  http_ud_t *ud = check_http(L, 1);
  return set_http_cb(L, &ud->cb_closed);
}

// Methods bound via HTTP_MT.__index
static const luaL_Reg l_http_methods[] = {
    {"close", l_http_close},
    {"setKeepAlive", l_http_setKeepAlive},
    {"setByteRange", l_http_setByteRange},
    {"setConnectTimeout", l_http_setConnectTimeout},
    {"setReadTimeout", l_http_setReadTimeout},
    {"setReadBufferSize", l_http_setReadBufferSize},
    {"get", l_http_get},
    {"post", l_http_post},
    {"getError", l_http_getError},
    {"getProgress", l_http_getProgress},
    {"getBytesAvailable", l_http_getBytesAvailable},
    {"read", l_http_read},
    {"getResponseStatus", l_http_getResponseStatus},
    {"getResponseHeaders", l_http_getResponseHeaders},
    {"setRequestCallback", l_http_setRequestCallback},
    {"setHeadersReadCallback", l_http_setHeadersReadCallback},
    {"setRequestCompleteCallback", l_http_setRequestCompleteCallback},
    {"setConnectionClosedCallback", l_http_setConnectionClosedCallback},
    {NULL, NULL}};

// Constructor table (picocalc.network.http)
static const luaL_Reg l_http_lib[] = {{"new", l_http_new}, {NULL, NULL}};

// ── picocalc.network functions
// ────────────────────────────────────────────────

// picocalc.network.setEnabled(flag, [callback])
static int l_network_setEnabled(lua_State *L) {
  bool flag = lua_toboolean(L, 1);
  if (flag) {
    // Re-connect if idle; use stored credentials
    wifi_status_t st = wifi_get_status();
    if (st == WIFI_STATUS_DISCONNECTED || st == WIFI_STATUS_FAILED) {
      const char *ssid = config_get("wifi_ssid");
      const char *pass = config_get("wifi_pass");
      if (ssid && ssid[0])
        wifi_connect(ssid, pass ? pass : "");
    }
  } else {
    wifi_disconnect();
  }
  // Optional callback(error_string_or_nil) — fire synchronously with nil
  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushnil(L);
    lua_pcall(L, 1, 0, 0);
  }
  return 0;
}

// picocalc.network.getStatus() -> kStatus* constant
static int l_network_getStatus(lua_State *L) {
  if (!wifi_is_available()) {
    lua_pushinteger(L, 2);
    return 1;
  } // kStatusNotAvailable
  wifi_status_t st = wifi_get_status();
  int ret = 0;
  switch (st) {
  case WIFI_STATUS_CONNECTED:
    ret = 1;
    break; // kStatusConnected
  case WIFI_STATUS_CONNECTING:
    ret = 0;
    break; // kStatusNotConnected
  case WIFI_STATUS_FAILED:
    ret = 2;
    break; // kStatusNotAvailable
  default:
    ret = 0;
    break; // kStatusNotConnected
  }
  // Only log on changes or occasionally if needed, but for now log always to
  // catch the issue printf("[LUA] network.getStatus() -> %d (wifi_st=%d)\n",
  // ret, (int)st);
  lua_pushinteger(L, ret);
  return 1;
}

static const luaL_Reg l_network_lib[] = {{"setEnabled", l_network_setEnabled},
                                         {"getStatus", l_network_getStatus},
                                         {NULL, NULL}};


// We need l_wifi_lib here too or we can just make a lua_bridge_wifi.c, but there was no wifi.c
// User asked to merge them or put wifi in network? User said:
// `lua_bridge_network.c` | ~550 | WiFi bindings, HTTP userdata, `http_lua_fire_pending`, all HTTP methods

static void on_http_slot_free(void *lua_ud) {
  if (lua_ud) {
    http_ud_t *ud = (http_ud_t *)lua_ud;
    ud->conn = NULL;
  }
}

void lua_bridge_network_init(lua_State *L) {

  http_close_all(on_http_slot_free);

  register_subtable(L, "wifi", l_wifi_lib);
  // Push WiFi status constants into picocalc.wifi
  lua_getfield(L, -1, "wifi");
  lua_pushinteger(L, WIFI_STATUS_DISCONNECTED); lua_setfield(L, -2, "STATUS_DISCONNECTED");
  lua_pushinteger(L, WIFI_STATUS_CONNECTING); lua_setfield(L, -2, "STATUS_CONNECTING");
  lua_pushinteger(L, WIFI_STATUS_CONNECTED); lua_setfield(L, -2, "STATUS_CONNECTED");
  lua_pushinteger(L, WIFI_STATUS_FAILED); lua_setfield(L, -2, "STATUS_FAILED");
  lua_pop(L, 1);

  // Install HTTP connection metatable
  luaL_newmetatable(L, HTTP_MT);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, l_http_methods, 0);
  lua_pushcfunction(L, l_http_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  // Build picocalc.network table
  lua_newtable(L);
  luaL_setfuncs(L, l_network_lib, 0);

  // Build picocalc.network.http table
  lua_newtable(L);
  luaL_setfuncs(L, l_http_lib, 0);
  lua_setfield(L, -2, "http");

  // Status constants on picocalc.network
  lua_pushinteger(L, 0); lua_setfield(L, -2, "kStatusNotConnected");
  lua_pushinteger(L, 1); lua_setfield(L, -2, "kStatusConnected");
  lua_pushinteger(L, 2); lua_setfield(L, -2, "kStatusNotAvailable");

  lua_setfield(L, -2, "network");
}
