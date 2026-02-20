#include "lua_bridge.h"
#include "../os/os.h"
#include "../os/system_menu.h"
#include "../os/screenshot.h"
#include "../os/config.h"
#include "../os/clock.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/wifi.h"
#include "../drivers/http.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/watchdog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── Colour helpers ────────────────────────────────────────────────────────────
// Lua passes colours as RGB565 integers (or we provide helper constructors)

static uint16_t l_checkcolor(lua_State *L, int idx) {
    return (uint16_t)luaL_checkinteger(L, idx);
}

// ── picocalc.display.* ───────────────────────────────────────────────────────

static int l_display_clear(lua_State *L) {
    uint16_t color = (lua_gettop(L) >= 1) ? l_checkcolor(L, 1) : COLOR_BLACK;
    display_clear(color);
    return 0;
}

static int l_display_setPixel(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    uint16_t c = l_checkcolor(L, 3);
    display_set_pixel(x, y, c);
    return 0;
}

static int l_display_fillRect(lua_State *L) {
    display_fill_rect(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
                      luaL_checkinteger(L, 3), luaL_checkinteger(L, 4),
                      l_checkcolor(L, 5));
    return 0;
}

static int l_display_drawRect(lua_State *L) {
    display_draw_rect(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
                      luaL_checkinteger(L, 3), luaL_checkinteger(L, 4),
                      l_checkcolor(L, 5));
    return 0;
}

static int l_display_drawLine(lua_State *L) {
    display_draw_line(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
                      luaL_checkinteger(L, 3), luaL_checkinteger(L, 4),
                      l_checkcolor(L, 5));
    return 0;
}

static int l_display_drawText(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    const char *text = luaL_checkstring(L, 3);
    uint16_t fg = l_checkcolor(L, 4);
    uint16_t bg = (lua_gettop(L) >= 5) ? l_checkcolor(L, 5) : COLOR_BLACK;
    int width = display_draw_text(x, y, text, fg, bg);
    lua_pushinteger(L, width);
    return 1;
}

static int l_display_flush(lua_State *L) {
    (void)L;
    display_flush();
    return 0;
}

static int l_display_getWidth(lua_State *L) {
    lua_pushinteger(L, FB_WIDTH);
    return 1;
}

static int l_display_getHeight(lua_State *L) {
    lua_pushinteger(L, FB_HEIGHT);
    return 1;
}

static int l_display_setBrightness(lua_State *L) {
    display_set_brightness((uint8_t)luaL_checkinteger(L, 1));
    return 0;
}

static int l_display_textWidth(lua_State *L) {
    lua_pushinteger(L, display_text_width(luaL_checkstring(L, 1)));
    return 1;
}

// Convenience: create RGB565 from r,g,b components
static int l_display_rgb(lua_State *L) {
    int r = luaL_checkinteger(L, 1);
    int g = luaL_checkinteger(L, 2);
    int b = luaL_checkinteger(L, 3);
    lua_pushinteger(L, RGB565(r, g, b));
    return 1;
}

static const luaL_Reg l_display_lib[] = {
    {"clear",         l_display_clear},
    {"setPixel",      l_display_setPixel},
    {"fillRect",      l_display_fillRect},
    {"drawRect",      l_display_drawRect},
    {"drawLine",      l_display_drawLine},
    {"drawText",      l_display_drawText},
    {"flush",         l_display_flush},
    {"getWidth",      l_display_getWidth},
    {"getHeight",     l_display_getHeight},
    {"setBrightness", l_display_setBrightness},
    {"textWidth",     l_display_textWidth},
    {"rgb",           l_display_rgb},
    {NULL, NULL}
};

// ── picocalc.input.* ─────────────────────────────────────────────────────────

static int l_input_getButtons(lua_State *L) {
    lua_pushinteger(L, kbd_get_buttons());
    return 1;
}

static int l_input_getButtonsPressed(lua_State *L) {
    lua_pushinteger(L, kbd_get_buttons_pressed());
    return 1;
}

static int l_input_getButtonsReleased(lua_State *L) {
    lua_pushinteger(L, kbd_get_buttons_released());
    return 1;
}

static int l_input_getChar(lua_State *L) {
    char c = kbd_get_char();
    if (c) {
        char s[2] = {c, '\0'};
        lua_pushstring(L, s);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_input_update(lua_State *L) {
    (void)L;
    kbd_poll();
    return 0;
}

static int l_input_getRawKey(lua_State *L) {
    lua_pushinteger(L, kbd_get_raw_key());
    return 1;
}

static const luaL_Reg l_input_lib[] = {
    {"update",             l_input_update},
    {"getButtons",         l_input_getButtons},
    {"getButtonsPressed",  l_input_getButtonsPressed},
    {"getButtonsReleased", l_input_getButtonsReleased},
    {"getChar",            l_input_getChar},
    {"getRawKey",          l_input_getRawKey},
    {NULL, NULL}
};

// ── picocalc.sys.* ───────────────────────────────────────────────────────────

static int l_sys_getTimeMs(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)to_ms_since_boot(get_absolute_time()));
    return 1;
}

static int l_sys_getBattery(lua_State *L) {
    // Battery reads are slow I2C round-trips — cache for 5 seconds.
    static int      s_cached = -1;
    static uint32_t s_last_ms = 0;
    uint32_t now = (uint32_t)to_ms_since_boot(get_absolute_time());
    if (s_last_ms == 0 || now - s_last_ms >= 5000) {
        s_cached  = kbd_get_battery_percent();
        s_last_ms = now;
    }
    lua_pushinteger(L, s_cached);
    return 1;
}

static int l_sys_log(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    printf("[APP] %s\n", msg);
    return 0;
}

static int l_sys_sleep(lua_State *L) {
    int ms = (int)luaL_checkinteger(L, 1);
    // Do NOT call kbd_poll() here — it would drain the STM32 FIFO and consume
    // character/button events that the app expects to read via input.update().
    // The Lua instruction hook (fires every 256 opcodes) handles menu detection
    // immediately after sleep returns.
    uint32_t end_ms = (uint32_t)to_ms_since_boot(get_absolute_time()) + (uint32_t)ms;
    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now >= end_ms) break;
        uint32_t remaining = end_ms - now;
        sleep_ms(remaining < 10 ? remaining : 10);
    }
    return 0;
}

static int l_sys_reboot(lua_State *L) {
    (void)L;
    watchdog_enable(1, true);
    for (;;) tight_loop_contents();
    return 0;
}

static int l_sys_isUSBPowered(lua_State *L) {
    // RP2350: VBUS presence is readable via USB hardware; implement if needed.
    // Stub returns false for now.
    lua_pushboolean(L, false);
    return 1;
}

// Exit the current app cleanly, returning to the launcher.
// Equivalent to `return` at the top level of main.lua, but works from
// any call depth. The launcher detects the sentinel and skips the error screen.
static int l_sys_exit(lua_State *L) {
    (void)L;
    return luaL_error(L, "__picocalc_exit__");
}

// ── picocalc.sys.addMenuItem / clearMenuItems ─────────────────────────────────
// Lua-registered callbacks are stored here as Lua registry references.
// A C trampoline is passed to system_menu_add_item() so that calling the
// menu item invokes the original Lua function.

typedef struct {
    lua_State *L;
    int        ref;   // LUA_REGISTRYINDEX reference to the Lua function
} lua_callback_t;

static lua_callback_t s_lua_callbacks[SYSMENU_MAX_APP_ITEMS];
static int            s_lua_callback_count = 0;

static void lua_menu_trampoline(void *user) {
    lua_callback_t *cb = (lua_callback_t *)user;
    lua_rawgeti(cb->L, LUA_REGISTRYINDEX, cb->ref);
    lua_call(cb->L, 0, 0);  // propagates errors (including sys.exit() sentinel)
}

// picocalc.sys.getClock() → {synced=bool, hour=int, min=int, sec=int, epoch=int}
// epoch is UTC Unix seconds; hour/min/sec are UTC + tz_offset.
static int l_sys_getClock(lua_State *L) {
    int h = 0, m = 0, s = 0;
    bool synced = clock_get_time(&h, &m, &s);
    lua_createtable(L, 0, 5);
    lua_pushboolean(L, synced);   lua_setfield(L, -2, "synced");
    lua_pushinteger(L, h);         lua_setfield(L, -2, "hour");
    lua_pushinteger(L, m);         lua_setfield(L, -2, "min");
    lua_pushinteger(L, s);         lua_setfield(L, -2, "sec");
    lua_pushinteger(L, (lua_Integer)clock_get_epoch());
                                   lua_setfield(L, -2, "epoch");
    return 1;
}

static int l_sys_addMenuItem(lua_State *L) {
    const char *label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (s_lua_callback_count >= SYSMENU_MAX_APP_ITEMS)
        return luaL_error(L, "too many menu items (max %d)", SYSMENU_MAX_APP_ITEMS);

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_callback_t *cb = &s_lua_callbacks[s_lua_callback_count++];
    cb->L   = L;
    cb->ref = ref;

    system_menu_add_item(label, lua_menu_trampoline, cb);
    return 0;
}

static int l_sys_clearMenuItems(lua_State *L) {
    for (int i = 0; i < s_lua_callback_count; i++)
        luaL_unref(L, LUA_REGISTRYINDEX, s_lua_callbacks[i].ref);
    s_lua_callback_count = 0;
    system_menu_clear_items();
    return 0;
}

static const luaL_Reg l_sys_lib[] = {
    {"getTimeMs",      l_sys_getTimeMs},
    {"getBattery",     l_sys_getBattery},
    {"log",            l_sys_log},
    {"sleep",          l_sys_sleep},
    {"exit",           l_sys_exit},
    {"reboot",         l_sys_reboot},
    {"isUSBPowered",   l_sys_isUSBPowered},
    {"getClock",       l_sys_getClock},
    {"addMenuItem",    l_sys_addMenuItem},
    {"clearMenuItems", l_sys_clearMenuItems},
    {NULL, NULL}
};

// ── picocalc.fs.* ────────────────────────────────────────────────────────────
// Thin wrapper over sdcard_ functions, exposed to Lua

#include "../drivers/sdcard.h"
#include "file_browser.h"

// ── Filesystem sandbox ────────────────────────────────────────────────────────
// Apps are allowed to access only two trees:
//   /apps/<dirname>/  — read-only (their own app bundle)
//   /data/<dirname>/  — read + write (their own data directory)
//
// <dirname> is derived from the APP_DIR global set by launcher.c, e.g.
//   APP_DIR = "/apps/editor"  → dirname = "editor"
//
// Relative paths and any path containing ".." are always rejected.

static bool fs_sandbox_check(lua_State *L, const char *path, bool write) {
    if (!path || path[0] != '/')  return false;  // require absolute paths
    if (strstr(path, ".."))       return false;  // reject traversal

    lua_getglobal(L, "APP_DIR");
    const char *app_dir = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (!app_dir) return false;

    // Extract the directory name component from "/apps/<dirname>"
    const char *dirname = strrchr(app_dir, '/');
    if (!dirname || dirname[1] == '\0') return false;
    dirname++;   // skip the '/'

    // /data/<dirname> prefix (no trailing slash — also matches the dir itself)
    char data_prefix[128];
    int dp_len = snprintf(data_prefix, sizeof(data_prefix), "/data/%s", dirname);
    bool in_data = (strncmp(path, data_prefix, dp_len) == 0 &&
                    (path[dp_len] == '\0' || path[dp_len] == '/'));

    if (write) return in_data;

    // For reads also allow /apps/<dirname>/...
    char app_prefix[128];
    int ap_len = snprintf(app_prefix, sizeof(app_prefix), "/apps/%s/", dirname);
    bool in_app = (strncmp(path, app_prefix, ap_len) == 0);

    return in_data || in_app;
}

static int l_fs_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *mode = luaL_optstring(L, 2, "r");
    bool needs_write = (strchr(mode, 'w') != NULL ||
                        strchr(mode, 'a') != NULL ||
                        strchr(mode, '+') != NULL);
    if (!fs_sandbox_check(L, path, needs_write)) { lua_pushnil(L); return 1; }
    sdfile_t f = sdcard_fopen(path, mode);
    if (!f) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, f);
    return 1;
}

static int l_fs_read(lua_State *L) {
    sdfile_t f = lua_touserdata(L, 1);
    int len = (int)luaL_checkinteger(L, 2);
    char *buf = (char *)malloc(len);
    if (!buf) { lua_pushnil(L); return 1; }
    int n = sdcard_fread(f, buf, len);
    if (n <= 0) { free(buf); lua_pushnil(L); return 1; }
    lua_pushlstring(L, buf, n);
    free(buf);
    return 1;
}

static int l_fs_write(lua_State *L) {
    sdfile_t f = lua_touserdata(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    int n = sdcard_fwrite(f, data, (int)len);
    lua_pushinteger(L, n);
    return 1;
}

static int l_fs_close(lua_State *L) {
    sdfile_t f = lua_touserdata(L, 1);
    sdcard_fclose(f);
    return 0;
}

static int l_fs_exists(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (!fs_sandbox_check(L, path, false)) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, sdcard_fexists(path));
    return 1;
}

static int l_fs_readFile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (!fs_sandbox_check(L, path, false)) { lua_pushnil(L); return 1; }
    int len = 0;
    char *buf = sdcard_read_file(path, &len);
    if (!buf) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, buf, len);
    free(buf);
    return 1;
}

static int l_fs_size(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (!fs_sandbox_check(L, path, false)) { lua_pushinteger(L, -1); return 1; }
    lua_pushinteger(L, sdcard_fsize(path));
    return 1;
}

// Context passed through sdcard_list_dir's void* user pointer
typedef struct { lua_State *L; int tidx; int n; } listdir_ctx_t;

static void listdir_cb(const sdcard_entry_t *e, void *user) {
    listdir_ctx_t *ctx = (listdir_ctx_t *)user;
    lua_State *L = ctx->L;
    lua_newtable(L);
    lua_pushstring(L,  e->name);    lua_setfield(L, -2, "name");
    lua_pushboolean(L, e->is_dir);  lua_setfield(L, -2, "is_dir");
    lua_pushinteger(L, e->size);    lua_setfield(L, -2, "size");
    lua_rawseti(L, ctx->tidx, ++ctx->n);
}

// Returns an array of {name, is_dir, size} tables, or an empty table on error.
static int l_fs_listDir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    lua_newtable(L);
    if (!fs_sandbox_check(L, path, false)) return 1;  // return empty table
    listdir_ctx_t ctx = { L, lua_gettop(L), 0 };
    sdcard_list_dir(path, listdir_cb, &ctx);
    return 1;
}

static int l_fs_mkdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (!fs_sandbox_check(L, path, true)) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, sdcard_mkdir(path));
    return 1;
}

// Convenience: return the path /data/<dirname>/<name>, auto-creating the
// data directory if it does not already exist.
// Usage: local path = picocalc.fs.appPath("save.json")
static int l_fs_appPath(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);

    lua_getglobal(L, "APP_DIR");
    const char *app_dir = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (!app_dir) { lua_pushnil(L); return 1; }

    const char *dirname = strrchr(app_dir, '/');
    if (!dirname || dirname[1] == '\0') { lua_pushnil(L); return 1; }
    dirname++;   // skip '/'

    // Auto-create /data/<dirname>/ on first call
    char data_dir[128];
    snprintf(data_dir, sizeof(data_dir), "/data/%s", dirname);
    sdcard_mkdir(data_dir);

    char full_path[192];
    snprintf(full_path, sizeof(full_path), "/data/%s/%s", dirname, name);
    lua_pushstring(L, full_path);
    return 1;
}

// Open a file-browser panel overlay.
// Optional arg: start directory (defaults to the app's /data/<dirname>/ dir).
// Returns the selected file path as a string, or nil if cancelled.
static int l_fs_browse(lua_State *L) {
    const char *start_path;
    static char default_path[128];

    if (lua_isnoneornil(L, 1)) {
        lua_getglobal(L, "APP_DIR");
        const char *app_dir = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (app_dir) {
            const char *dirname = strrchr(app_dir, '/');
            if (dirname && dirname[1] != '\0') {
                dirname++;
                snprintf(default_path, sizeof(default_path), "/data/%s", dirname);
                sdcard_mkdir(default_path);
                start_path = default_path;
            } else {
                start_path = "/data";
            }
        } else {
            start_path = "/data";
        }
    } else {
        start_path = luaL_checkstring(L, 1);
    }

    char selected[192];
    if (file_browser_show(start_path, selected, sizeof(selected))) {
        lua_pushstring(L, selected);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static const luaL_Reg l_fs_lib[] = {
    {"open",     l_fs_open},
    {"read",     l_fs_read},
    {"write",    l_fs_write},
    {"close",    l_fs_close},
    {"exists",   l_fs_exists},
    {"readFile", l_fs_readFile},
    {"size",     l_fs_size},
    {"listDir",  l_fs_listDir},
    {"mkdir",    l_fs_mkdir},
    {"appPath",  l_fs_appPath},
    {"browse",   l_fs_browse},
    {NULL, NULL}
};

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
    if (ip) lua_pushstring(L, ip);
    else    lua_pushnil(L);
    return 1;
}

static int l_wifi_getSSID(lua_State *L) {
    const char *ssid = wifi_get_ssid();
    if (ssid) lua_pushstring(L, ssid);
    else      lua_pushnil(L);
    return 1;
}

static const luaL_Reg l_wifi_lib[] = {
    {"isAvailable", l_wifi_isAvailable},
    {"connect",     l_wifi_connect},
    {"disconnect",  l_wifi_disconnect},
    {"getStatus",   l_wifi_getStatus},
    {"getIP",       l_wifi_getIP},
    {"getSSID",     l_wifi_getSSID},
    {NULL, NULL}
};

// ── picocalc.network.* and picocalc.network.http.* ───────────────────────────
//
// picocalc.network.http.new() returns a Lua full-userdata object with method
// bindings via a metatable.  Callbacks are fired from menu_lua_hook (after
// wifi_poll() returns) — never from inside lwIP callbacks — so lua_pcall is
// always safe to call there.

#define HTTP_MT "picocalc.network.http"  // metatable registry key

// Lua userdata that wraps one http_conn_t slot from the C pool.
typedef struct {
    http_conn_t *conn;    // NULL once closed/GC'd
    int cb_request;       // LUA_NOREF or registry ref
    int cb_headers;
    int cb_complete;
    int cb_closed;
} http_ud_t;

// ── HTTP callback dispatcher (called from menu_lua_hook) ──────────────────────

// Iterates the C connection pool, reads & clears pending flags, and fires the
// corresponding Lua callbacks via lua_pcall.  Safe because we are OUTSIDE of
// wifi_poll() / cyw43_arch_poll() when this runs.
static void http_lua_fire_pending(lua_State *L) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        http_conn_t *c = http_get_conn(i);
        if (!c || !c->lua_ud) continue;

        uint8_t pend = http_take_pending(c);
        if (!pend) continue;

        http_ud_t *ud = (http_ud_t *)c->lua_ud;

        // Fire in order: headers → data → complete → closed
        if ((pend & HTTP_CB_HEADERS) && ud->cb_headers != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_headers);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) lua_pop(L, 1);
        }
        if ((pend & HTTP_CB_REQUEST) && ud->cb_request != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_request);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) lua_pop(L, 1);
        }
        if ((pend & HTTP_CB_COMPLETE) && ud->cb_complete != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_complete);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) lua_pop(L, 1);
        }
        if ((pend & (HTTP_CB_CLOSED | HTTP_CB_FAILED)) && ud->cb_closed != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_closed);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) lua_pop(L, 1);
        }
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static http_ud_t *check_http(lua_State *L, int idx) {
    return (http_ud_t *)luaL_checkudata(L, idx, HTTP_MT);
}

static http_ud_t *check_http_open(lua_State *L, int idx) {
    http_ud_t *ud = check_http(L, idx);
    if (!ud->conn) luaL_error(L, "http: connection is closed");
    return ud;
}

// Convert a Lua headers argument (string / array / kv-table) at stack index
// `idx` to a malloc'd "Key: Value\r\n..." string, or NULL if nil/absent.
static char *lua_headers_to_str(lua_State *L, int idx) {
    if (lua_isnoneornil(L, idx)) return NULL;

    char *buf = malloc(4096);
    if (!buf) return NULL;
    int n = 0;

    if (lua_isstring(L, idx)) {
        const char *s = lua_tostring(L, idx);
        n = snprintf(buf, 4096, "%s", s);
        if (n >= 2 && (buf[n-2] != '\r' || buf[n-1] != '\n'))
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
                    if (n >= 2 && (buf[n-2] != '\r' || buf[n-1] != '\n'))
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

    if (n == 0) { free(buf); return NULL; }
    return buf;
}

// Unref all callback registry entries on a userdata.
static void http_ud_unref_all(lua_State *L, http_ud_t *ud) {
    if (ud->cb_request  != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_request);  ud->cb_request  = LUA_NOREF; }
    if (ud->cb_headers  != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_headers);  ud->cb_headers  = LUA_NOREF; }
    if (ud->cb_complete != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_complete); ud->cb_complete = LUA_NOREF; }
    if (ud->cb_closed   != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_closed);   ud->cb_closed   = LUA_NOREF; }
}

// ── Metatable methods ──────────────────────────────────────────────────────────

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

// picocalc.network.http.new(server, [port], [usessl], [reason]) -> obj or nil, err
static int l_http_new(lua_State *L) {
    const char *server = luaL_checkstring(L, 1);
    bool use_ssl = lua_gettop(L) >= 3 && lua_toboolean(L, 3);

    if (use_ssl) {
        lua_pushnil(L);
        lua_pushstring(L, "SSL not supported on PicOS");
        return 2;
    }

    lua_Integer port = luaL_optinteger(L, 2, 80);

    http_conn_t *conn = http_alloc();
    if (!conn) {
        lua_pushnil(L);
        lua_pushstring(L, "HTTP connection pool full or out of memory");
        return 2;
    }

    strncpy(conn->server, server, HTTP_SERVER_MAX - 1);
    conn->port = (uint16_t)port;

    http_ud_t *ud = (http_ud_t *)lua_newuserdata(L, sizeof(http_ud_t));
    ud->conn        = conn;
    ud->cb_request  = LUA_NOREF;
    ud->cb_headers  = LUA_NOREF;
    ud->cb_complete = LUA_NOREF;
    ud->cb_closed   = LUA_NOREF;
    conn->lua_ud    = ud;

    luaL_getmetatable(L, HTTP_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// conn:close()
static int l_http_close(lua_State *L) {
    http_ud_t *ud = check_http(L, 1);
    if (ud->conn) http_close(ud->conn);
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
    ud->conn->range_to   = (int32_t)luaL_checkinteger(L, 3);
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
    http_ud_t  *ud   = check_http_open(L, 1);
    const char *path = luaL_checkstring(L, 2);

    char       *hdrs     = NULL;
    const char *body     = NULL;
    size_t      body_len = 0;
    int         nargs    = lua_gettop(L);

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
        if (nargs >= 3) hdrs = lua_headers_to_str(L, 3);
    }

    bool ok = has_body
        ? http_post(ud->conn, path, hdrs, body, body_len)
        : http_get (ud->conn, path, hdrs);

    free(hdrs);

    lua_pushboolean(L, ok);
    if (!ok) { lua_pushstring(L, ud->conn->err); return 2; }
    return 1;
}

static int l_http_get(lua_State *L)  { return do_request(L, false); }
static int l_http_post(lua_State *L) { return do_request(L, true);  }

// conn:getError() -> string or nil
static int l_http_getError(lua_State *L) {
    http_ud_t *ud = check_http(L, 1);
    if (!ud->conn || ud->conn->err[0] == '\0') { lua_pushnil(L); return 1; }
    lua_pushstring(L, ud->conn->err);
    return 1;
}

// conn:getProgress() -> bytes_received, total (-1 if unknown)
static int l_http_getProgress(lua_State *L) {
    http_ud_t *ud = check_http(L, 1);
    lua_pushinteger(L, ud->conn ? (lua_Integer)ud->conn->body_received  : 0);
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
    if (!ud->conn) { lua_pushnil(L); return 1; }

    uint32_t avail = http_bytes_available(ud->conn);
    if (avail == 0) { lua_pushnil(L); return 1; }

    uint32_t want = avail;
    if (!lua_isnoneornil(L, 2)) {
        lua_Integer req = luaL_checkinteger(L, 2);
        if (req > 0 && (uint32_t)req < want) want = (uint32_t)req;
    }
    if (want > 65536) want = 65536;

    uint8_t *tmp = malloc(want);
    if (!tmp) { lua_pushnil(L); return 1; }

    uint32_t n = http_read(ud->conn, tmp, want);
    if (n > 0)  lua_pushlstring(L, (char *)tmp, n);
    else        lua_pushnil(L);
    free(tmp);
    return 1;
}

// conn:getResponseStatus() -> integer or nil
static int l_http_getResponseStatus(lua_State *L) {
    http_ud_t *ud = check_http(L, 1);
    if (!ud->conn || ud->conn->status_code == 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, ud->conn->status_code);
    return 1;
}

// conn:getResponseHeaders() -> table {key=value} or nil
static int l_http_getResponseHeaders(lua_State *L) {
    http_ud_t *ud = check_http(L, 1);
    if (!ud->conn || !ud->conn->headers_done) { lua_pushnil(L); return 1; }

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
    if (!lua_isnoneornil(L, 2)) luaL_checktype(L, 2, LUA_TFUNCTION);
    if (*ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, *ref); *ref = LUA_NOREF; }
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
    {"close",                       l_http_close},
    {"setKeepAlive",                l_http_setKeepAlive},
    {"setByteRange",                l_http_setByteRange},
    {"setConnectTimeout",           l_http_setConnectTimeout},
    {"setReadTimeout",              l_http_setReadTimeout},
    {"setReadBufferSize",           l_http_setReadBufferSize},
    {"get",                         l_http_get},
    {"post",                        l_http_post},
    {"getError",                    l_http_getError},
    {"getProgress",                 l_http_getProgress},
    {"getBytesAvailable",           l_http_getBytesAvailable},
    {"read",                        l_http_read},
    {"getResponseStatus",           l_http_getResponseStatus},
    {"getResponseHeaders",          l_http_getResponseHeaders},
    {"setRequestCallback",          l_http_setRequestCallback},
    {"setHeadersReadCallback",      l_http_setHeadersReadCallback},
    {"setRequestCompleteCallback",  l_http_setRequestCompleteCallback},
    {"setConnectionClosedCallback", l_http_setConnectionClosedCallback},
    {NULL, NULL}
};

// Constructor table (picocalc.network.http)
static const luaL_Reg l_http_lib[] = {
    {"new", l_http_new},
    {NULL, NULL}
};

// ── picocalc.network functions ────────────────────────────────────────────────

// picocalc.network.setEnabled(flag, [callback])
static int l_network_setEnabled(lua_State *L) {
    bool flag = lua_toboolean(L, 1);
    if (flag) {
        // Re-connect if idle; use stored credentials
        wifi_status_t st = wifi_get_status();
        if (st == WIFI_STATUS_DISCONNECTED || st == WIFI_STATUS_FAILED) {
            const char *ssid = config_get("wifi_ssid");
            const char *pass = config_get("wifi_pass");
            if (ssid && ssid[0]) wifi_connect(ssid, pass ? pass : "");
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
    if (!wifi_is_available()) { lua_pushinteger(L, 2); return 1; } // kStatusNotAvailable
    switch (wifi_get_status()) {
        case WIFI_STATUS_CONNECTED: lua_pushinteger(L, 1); break; // kStatusConnected
        case WIFI_STATUS_FAILED:    lua_pushinteger(L, 2); break; // kStatusNotAvailable
        default:                    lua_pushinteger(L, 0); break; // kStatusNotConnected
    }
    return 1;
}

static const luaL_Reg l_network_lib[] = {
    {"setEnabled", l_network_setEnabled},
    {"getStatus",  l_network_getStatus},
    {NULL, NULL}
};

// ── picocalc.config.* ─────────────────────────────────────────────────────────

static int l_config_get(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    const char *val = config_get(key);
    if (val) lua_pushstring(L, val);
    else     lua_pushnil(L);
    return 1;
}

static int l_config_set(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    // Allow nil/absent second arg to delete the key
    const char *val = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
    config_set(key, val);
    return 0;
}

static int l_config_save(lua_State *L) {
    lua_pushboolean(L, config_save());
    return 1;
}

static int l_config_load(lua_State *L) {
    lua_pushboolean(L, config_load());
    return 1;
}

static const luaL_Reg l_config_lib[] = {
    {"get",  l_config_get},
    {"set",  l_config_set},
    {"save", l_config_save},
    {"load", l_config_load},
    {NULL, NULL}
};

// ── picocalc.perf.* ──────────────────────────────────────────────────────────
// Performance monitoring utilities for apps

#define PERF_SAMPLES 30

static uint32_t s_perf_frame_times[PERF_SAMPLES] = {0};
static int      s_perf_index = 0;
static uint32_t s_perf_frame_start = 0;
static uint32_t s_perf_last_frame_time = 0;
static int      s_perf_fps = 0;

// Start timing a frame. Call at the beginning of your game loop.
static int l_perf_beginFrame(lua_State *L) {
    (void)L;
    s_perf_frame_start = to_ms_since_boot(get_absolute_time());
    return 0;
}

// End timing a frame and calculate FPS. Call at the end of your game loop.
static int l_perf_endFrame(lua_State *L) {
    (void)L;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t delta = now - s_perf_frame_start;
    
    s_perf_last_frame_time = delta;
    s_perf_frame_times[s_perf_index] = delta;
    s_perf_index = (s_perf_index + 1) % PERF_SAMPLES;
    
    // Calculate average frame time
    uint32_t sum = 0;
    for (int i = 0; i < PERF_SAMPLES; i++) {
        sum += s_perf_frame_times[i];
    }
    uint32_t avg_frame_time = sum / PERF_SAMPLES;
    
    // Calculate FPS (avoid divide by zero)
    s_perf_fps = (avg_frame_time > 0) ? (1000 / avg_frame_time) : 0;
    
    return 0;
}

// Get current FPS (averaged over recent frames)
static int l_perf_getFPS(lua_State *L) {
    lua_pushinteger(L, s_perf_fps);
    return 1;
}

// Get last frame time in milliseconds
static int l_perf_getFrameTime(lua_State *L) {
    lua_pushinteger(L, s_perf_last_frame_time);
    return 1;
}

// Convenience: draw FPS counter at specified position with color coding
static int l_perf_drawFPS(lua_State *L) {
    int x = (int)luaL_optinteger(L, 1, 250);  // default top-right
    int y = (int)luaL_optinteger(L, 2, 8);
    
    char buf[16];
    snprintf(buf, sizeof(buf), "FPS: %d", s_perf_fps);
    
    // Color code: green >= 55, yellow >= 30, red < 30
    uint16_t color = (s_perf_fps >= 55) ? COLOR_GREEN :
                     (s_perf_fps >= 30) ? COLOR_YELLOW : COLOR_RED;
    
    display_draw_text(x, y, buf, color, COLOR_BLACK);
    return 0;
}

static const luaL_Reg l_perf_lib[] = {
    {"beginFrame",  l_perf_beginFrame},
    {"endFrame",    l_perf_endFrame},
    {"getFPS",      l_perf_getFPS},
    {"getFrameTime", l_perf_getFrameTime},
    {"drawFPS",     l_perf_drawFPS},
    {NULL, NULL}
};

// ── Registration ──────────────────────────────────────────────────────────────

// Create a sub-table from a luaL_Reg and attach it to the `picocalc` table
// that is already on the stack at index -1.
static void register_subtable(lua_State *L, const char *name,
                               const luaL_Reg *funcs) {
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_setfield(L, -2, name);
}

// Instruction-count hook: fires every 256 Lua opcodes.
// Drives the WiFi state machine and checks for the system menu button.
static void menu_lua_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    wifi_poll();
    http_lua_fire_pending(L);   // fire any queued HTTP Lua callbacks
    if (kbd_consume_menu_press())       system_menu_show(L);
    if (kbd_consume_screenshot_press()) screenshot_save();
}

void lua_bridge_register(lua_State *L) {
    // Reset per-app menu state before registering a new app
    s_lua_callback_count = 0;
    system_menu_clear_items();

    // Close any HTTP connections leaked by the previous app.
    // Normally __gc handles this, but http_close_all() is a safety net.
    http_close_all();

    // Open standard Lua libs (but not io/os/package for sandboxing)
    luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);

    // Create the top-level `picocalc` table
    lua_newtable(L);

    register_subtable(L, "display", l_display_lib);
    register_subtable(L, "input",   l_input_lib);
    register_subtable(L, "sys",     l_sys_lib);
    register_subtable(L, "fs",      l_fs_lib);
    register_subtable(L, "perf",    l_perf_lib);
    register_subtable(L, "wifi",    l_wifi_lib);
    register_subtable(L, "config",  l_config_lib);

    // Push button constants into picocalc.input
    lua_getfield(L, -1, "input");
    lua_pushinteger(L, BTN_UP);        lua_setfield(L, -2, "BTN_UP");
    lua_pushinteger(L, BTN_DOWN);      lua_setfield(L, -2, "BTN_DOWN");
    lua_pushinteger(L, BTN_LEFT);      lua_setfield(L, -2, "BTN_LEFT");
    lua_pushinteger(L, BTN_RIGHT);     lua_setfield(L, -2, "BTN_RIGHT");
    lua_pushinteger(L, BTN_ENTER);     lua_setfield(L, -2, "BTN_ENTER");
    lua_pushinteger(L, BTN_ESC);       lua_setfield(L, -2, "BTN_ESC");
    lua_pushinteger(L, BTN_MENU);      lua_setfield(L, -2, "BTN_MENU");
    lua_pushinteger(L, BTN_F1);        lua_setfield(L, -2, "BTN_F1");
    lua_pushinteger(L, BTN_F2);        lua_setfield(L, -2, "BTN_F2");
    lua_pushinteger(L, BTN_F3);        lua_setfield(L, -2, "BTN_F3");
    lua_pushinteger(L, BTN_F4);        lua_setfield(L, -2, "BTN_F4");
    lua_pushinteger(L, BTN_F5);        lua_setfield(L, -2, "BTN_F5");
    lua_pushinteger(L, BTN_F6);        lua_setfield(L, -2, "BTN_F6");
    lua_pushinteger(L, BTN_F7);        lua_setfield(L, -2, "BTN_F7");
    lua_pushinteger(L, BTN_F8);        lua_setfield(L, -2, "BTN_F8");
    lua_pushinteger(L, BTN_F9);        lua_setfield(L, -2, "BTN_F9");
    lua_pushinteger(L, BTN_BACKSPACE); lua_setfield(L, -2, "BTN_BACKSPACE");
    lua_pushinteger(L, BTN_TAB);       lua_setfield(L, -2, "BTN_TAB");
    lua_pushinteger(L, BTN_DEL);       lua_setfield(L, -2, "BTN_DEL");
    lua_pushinteger(L, BTN_SHIFT);     lua_setfield(L, -2, "BTN_SHIFT");
    lua_pushinteger(L, BTN_CTRL);      lua_setfield(L, -2, "BTN_CTRL");
    lua_pushinteger(L, BTN_ALT);       lua_setfield(L, -2, "BTN_ALT");
    lua_pushinteger(L, BTN_FN);        lua_setfield(L, -2, "BTN_FN");
    lua_pop(L, 1);  // pop input subtable

    // Push colour constants into picocalc.display
    lua_getfield(L, -1, "display");
    lua_pushinteger(L, COLOR_BLACK);  lua_setfield(L, -2, "BLACK");
    lua_pushinteger(L, COLOR_WHITE);  lua_setfield(L, -2, "WHITE");
    lua_pushinteger(L, COLOR_RED);    lua_setfield(L, -2, "RED");
    lua_pushinteger(L, COLOR_GREEN);  lua_setfield(L, -2, "GREEN");
    lua_pushinteger(L, COLOR_BLUE);   lua_setfield(L, -2, "BLUE");
    lua_pushinteger(L, COLOR_YELLOW); lua_setfield(L, -2, "YELLOW");
    lua_pushinteger(L, COLOR_CYAN);   lua_setfield(L, -2, "CYAN");
    lua_pushinteger(L, COLOR_GRAY);   lua_setfield(L, -2, "GRAY");
    lua_pop(L, 1);  // pop display subtable

    // Push WiFi status constants into picocalc.wifi
    lua_getfield(L, -1, "wifi");
    lua_pushinteger(L, WIFI_STATUS_DISCONNECTED); lua_setfield(L, -2, "STATUS_DISCONNECTED");
    lua_pushinteger(L, WIFI_STATUS_CONNECTING);   lua_setfield(L, -2, "STATUS_CONNECTING");
    lua_pushinteger(L, WIFI_STATUS_CONNECTED);    lua_setfield(L, -2, "STATUS_CONNECTED");
    lua_pushinteger(L, WIFI_STATUS_FAILED);       lua_setfield(L, -2, "STATUS_FAILED");
    lua_pop(L, 1);  // pop wifi subtable

    // ── picocalc.network (+ picocalc.network.http) ──────────────────────────

    // Install HTTP connection metatable (HTTP_MT) with all method bindings.
    // __index = metatable itself so conn:method() dispatch works.
    luaL_newmetatable(L, HTTP_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");    // HTTP_MT.__index = HTTP_MT
    luaL_setfuncs(L, l_http_methods, 0);
    lua_pushcfunction(L, l_http_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);  // pop metatable

    // Build picocalc.network table
    lua_newtable(L);
    luaL_setfuncs(L, l_network_lib, 0);   // setEnabled, getStatus

    // Build picocalc.network.http table (constructor + requestAccess)
    lua_newtable(L);
    luaL_setfuncs(L, l_http_lib, 0);
    lua_setfield(L, -2, "http");           // network.http = http table

    // Status constants on picocalc.network
    lua_pushinteger(L, 0); lua_setfield(L, -2, "kStatusNotConnected");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "kStatusConnected");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "kStatusNotAvailable");

    lua_setfield(L, -2, "network");   // picocalc.network = network table

    // Set as global
    lua_setglobal(L, "picocalc");

    // Install instruction-count hook for menu button interception.
    // Fires every 256 Lua opcodes (~100µs-1ms) to catch menu button presses
    // even during tight loops, without requiring apps to poll input.
    lua_sethook(L, menu_lua_hook, LUA_MASKCOUNT, 256);
}

void lua_bridge_show_error(lua_State *L, const char *context) {
    const char *err = lua_tostring(L, -1);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", err ? err : "unknown error");

    display_clear(COLOR_BLACK);
    display_draw_text(4, 4, context, COLOR_RED, COLOR_BLACK);

    // Word-wrap the error message at ~52 chars (320px / 6px per char)
    int col = 0, row = 1;
    char line[54] = {0};
    for (int i = 0; buf[i] && row < 38; i++) {
        line[col++] = buf[i];
        if (col >= 52 || buf[i] == '\n') {
            line[col] = '\0';
            display_draw_text(4, 4 + row * 9, line, COLOR_WHITE, COLOR_BLACK);
            row++; col = 0;
            memset(line, 0, sizeof(line));
        }
    }
    if (col > 0) {
        line[col] = '\0';
        display_draw_text(4, 4 + row * 9, line, COLOR_WHITE, COLOR_BLACK);
    }

    display_draw_text(4, FB_HEIGHT - 12, "Press Esc to continue", COLOR_GRAY, COLOR_BLACK);
    display_flush();

    // Drain any keys already held when the error occurred
    do {
        kbd_poll();
        sleep_ms(16);
    } while (kbd_get_buttons());

    // Wait specifically for Esc before returning
    while (true) {
        kbd_poll();
        uint32_t btns = kbd_get_buttons();
        if (btns & BTN_ESC) break;
        sleep_ms(16);
    }
    lua_pop(L, 1);
}
