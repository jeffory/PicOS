#include "lua_bridge_internal.h"
#include "../drivers/tcp.h"
#include "../drivers/wifi.h"
#include "umm_malloc.h"
#include "pico/time.h"
#include "hardware/timer.h"

#define TCP_MT "picocalc.tcp"

#define TCP_DEFAULT_CONNECT_TIMEOUT_MS 10000
#define TCP_DEFAULT_READ_TIMEOUT_MS   10000

typedef struct {
    tcp_conn_t *conn;
    uint32_t connect_timeout_ms;
    uint32_t read_timeout_ms;
    int cb_connect;
    int cb_read;
    int cb_closed;
} tcp_ud_t;

static tcp_ud_t *check_tcp(lua_State *L, int idx) {
    tcp_ud_t *ud = (tcp_ud_t *)luaL_checkudata(L, idx, TCP_MT);
    return ud;
}

static tcp_ud_t *check_tcp_open(lua_State *L, int idx) {
    tcp_ud_t *ud = check_tcp(L, idx);
    if (!ud->conn)
        luaL_error(L, "tcp: connection is closed");
    return ud;
}

static int set_tcp_cb(lua_State *L, int idx, int *ref_ptr) {
    if (lua_isnoneornil(L, idx)) {
        *ref_ptr = LUA_NOREF;
        return 0;
    }
    luaL_checktype(L, idx, LUA_TFUNCTION);
    lua_settop(L, idx);
    *ref_ptr = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static void tcp_ud_unref_all(lua_State *L, tcp_ud_t *ud) {
    if (!ud) return;
    if (ud->cb_connect != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_connect);
        ud->cb_connect = LUA_NOREF;
    }
    if (ud->cb_read != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_read);
        ud->cb_read = LUA_NOREF;
    }
    if (ud->cb_closed != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_closed);
        ud->cb_closed = LUA_NOREF;
    }
}

static int l_tcp_gc(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    tcp_ud_unref_all(L, ud);
    if (ud->conn) {
        tcp_free(ud->conn);
        ud->conn = NULL;
    }
    return 0;
}

static int l_tcp_new(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    uint16_t port = (uint16_t)luaL_optinteger(L, 2, 80);
    bool use_ssl = lua_gettop(L) >= 3 && lua_toboolean(L, 3);

    tcp_conn_t *conn = tcp_alloc();
    if (!conn) {
        lua_pushnil(L);
        lua_pushstring(L, "TCP connection pool full or out of memory");
        return 2;
    }

    strncpy(conn->host, host, sizeof(conn->host) - 1);
    conn->port = port;
    conn->use_ssl = use_ssl;

    tcp_ud_t *ud = (tcp_ud_t *)lua_newuserdata(L, sizeof(tcp_ud_t));
    ud->conn = conn;
    ud->connect_timeout_ms = TCP_DEFAULT_CONNECT_TIMEOUT_MS;
    ud->read_timeout_ms = TCP_DEFAULT_READ_TIMEOUT_MS;
    ud->cb_connect = LUA_NOREF;
    ud->cb_read = LUA_NOREF;
    ud->cb_closed = LUA_NOREF;

    luaL_getmetatable(L, TCP_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_tcp_connect(lua_State *L) {
    tcp_ud_t *ud = check_tcp_open(L, 1);
    if (!wifi_is_available()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "WiFi not available");
        return 2;
    }
    bool ok = tcp_connect(ud->conn, ud->conn->host, ud->conn->port, ud->conn->use_ssl);
    lua_pushboolean(L, ok);
    if (!ok) {
        lua_pushstring(L, ud->conn->err[0] ? ud->conn->err : "failed to queue connect");
    }
    return ok ? 1 : 2;
}

static int l_tcp_write(lua_State *L) {
    tcp_ud_t *ud = check_tcp_open(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    int written = tcp_write(ud->conn, data, (int)len);
    if (written < 0) {
        lua_pushinteger(L, -1);
        return 1;
    }
    lua_pushinteger(L, written);
    return 1;
}

static int l_tcp_read(lua_State *L) {
    tcp_ud_t *ud = check_tcp_open(L, 1);
    int max_len = (int)luaL_optinteger(L, 2, 4096);

    if (!ud->conn || ud->conn->state != TCP_STATE_CONNECTED) {
        lua_pushnil(L);
        return 1;
    }

    uint8_t *buf = umm_malloc((size_t)max_len + 1);
    if (!buf) {
        lua_pushnil(L);
        return 1;
    }

    int n = tcp_read(ud->conn, buf, max_len);
    if (n <= 0) {
        umm_free(buf);
        lua_pushnil(L);
        return 1;
    }

    buf[n] = '\0';
    lua_pushlstring(L, (const char *)buf, (size_t)n);
    umm_free(buf);
    return 1;
}

static int l_tcp_available(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    if (!ud->conn) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)tcp_bytes_available(ud->conn));
    return 1;
}

static int l_tcp_close(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    if (ud->conn) {
        tcp_close(ud->conn);
        tcp_free(ud->conn);
        ud->conn = NULL;
    }
    return 0;
}

static int l_tcp_error(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    if (!ud->conn || ud->conn->err[0] == '\0') {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, ud->conn->err);
    return 1;
}

static int l_tcp_isConnected(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    bool connected = ud->conn && ud->conn->state == TCP_STATE_CONNECTED;
    lua_pushboolean(L, connected);
    return 1;
}

static int l_tcp_setConnectTimeout(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    ud->connect_timeout_ms = (uint32_t)(luaL_checknumber(L, 2) * 1000.0);
    return 0;
}

static int l_tcp_setReadTimeout(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    ud->read_timeout_ms = (uint32_t)(luaL_checknumber(L, 2) * 1000.0);
    return 0;
}

static int l_tcp_setConnectCallback(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    return set_tcp_cb(L, 2, &ud->cb_connect);
}

static int l_tcp_setReadCallback(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    return set_tcp_cb(L, 2, &ud->cb_read);
}

static int l_tcp_setCloseCallback(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    return set_tcp_cb(L, 2, &ud->cb_closed);
}

static int l_tcp_getEvents(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    if (!ud->conn) {
        lua_pushinteger(L, 0);
        return 1;
    }
    uint32_t events = tcp_take_pending(ud->conn);
    lua_pushinteger(L, (lua_Integer)events);
    return 1;
}

static int l_tcp_waitConnected(lua_State *L) {
    tcp_ud_t *ud = check_tcp(L, 1);
    uint32_t timeout_ms = (uint32_t)(luaL_optnumber(L, 2, 10.0) * 1000.0);
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (ud->conn && ud->conn->state != TCP_STATE_CONNECTED) {
        if (ud->conn->state == TCP_STATE_FAILED || ud->conn->state == TCP_STATE_CLOSED) {
            break;
        }
        if (to_ms_since_boot(get_absolute_time()) - start > timeout_ms) {
            break;
        }
        sleep_ms(10);
    }

    lua_pushboolean(L, ud->conn && ud->conn->state == TCP_STATE_CONNECTED);
    return 1;
}

static int l_tcp_waitData(lua_State *L) {
    tcp_ud_t *ud = check_tcp_open(L, 1);
    uint32_t timeout_ms = (uint32_t)(luaL_optnumber(L, 2, 10.0) * 1000.0);
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (ud->conn && ud->conn->state == TCP_STATE_CONNECTED) {
        if (tcp_bytes_available(ud->conn) > 0) {
            break;
        }
        if (to_ms_since_boot(get_absolute_time()) - start > timeout_ms) {
            break;
        }
        sleep_ms(10);
    }

    lua_pushboolean(L, tcp_bytes_available(ud->conn) > 0);
    return 1;
}

static const luaL_Reg l_tcp_methods[] = {
    {"connect", l_tcp_connect},
    {"write", l_tcp_write},
    {"read", l_tcp_read},
    {"available", l_tcp_available},
    {"close", l_tcp_close},
    {"error", l_tcp_error},
    {"isConnected", l_tcp_isConnected},
    {"setConnectTimeout", l_tcp_setConnectTimeout},
    {"setReadTimeout", l_tcp_setReadTimeout},
    {"setConnectCallback", l_tcp_setConnectCallback},
    {"setReadCallback", l_tcp_setReadCallback},
    {"setCloseCallback", l_tcp_setCloseCallback},
    {"getEvents", l_tcp_getEvents},
    {"waitConnected", l_tcp_waitConnected},
    {"waitData", l_tcp_waitData},
    {NULL, NULL}
};

static const luaL_Reg l_tcp_lib[] = {
    {"new", l_tcp_new},
    {NULL, NULL}
};

void tcp_lua_fire_pending(lua_State *L) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *c = tcp_get_conn(i);
        if (!c || !c->in_use || !c->rx_buf) continue;

        uint32_t events = tcp_take_pending(c);
        if (events == 0) continue;
    }
}

void lua_bridge_tcp_init(lua_State *L) {
    luaL_newmetatable(L, TCP_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, l_tcp_methods, 0);
    lua_pushcfunction(L, l_tcp_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, l_tcp_lib, 0);

    lua_pushinteger(L, 1); lua_setfield(L, -2, "CB_CONNECT");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "CB_READ");
    lua_pushinteger(L, 4); lua_setfield(L, -2, "CB_WRITE");
    lua_pushinteger(L, 8); lua_setfield(L, -2, "CB_CLOSED");
    lua_pushinteger(L, 16); lua_setfield(L, -2, "CB_FAILED");

    lua_setfield(L, -2, "tcp");
}
