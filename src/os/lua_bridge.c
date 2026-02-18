#include "lua_bridge.h"
#include "../os/os.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"

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
    sleep_ms(ms);
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

static const luaL_Reg l_sys_lib[] = {
    {"getTimeMs",     l_sys_getTimeMs},
    {"getBattery",    l_sys_getBattery},
    {"log",           l_sys_log},
    {"sleep",         l_sys_sleep},
    {"exit",          l_sys_exit},
    {"reboot",        l_sys_reboot},
    {"isUSBPowered",  l_sys_isUSBPowered},
    {NULL, NULL}
};

// ── picocalc.fs.* ────────────────────────────────────────────────────────────
// Thin wrapper over sdcard_ functions, exposed to Lua

#include "../drivers/sdcard.h"

static int l_fs_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *mode = luaL_optstring(L, 2, "r");
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
    lua_pushboolean(L, sdcard_fexists(luaL_checkstring(L, 1)));
    return 1;
}

static int l_fs_readFile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int len = 0;
    char *buf = sdcard_read_file(path, &len);
    if (!buf) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, buf, len);
    free(buf);
    return 1;
}

static int l_fs_size(lua_State *L) {
    lua_pushinteger(L, sdcard_fsize(luaL_checkstring(L, 1)));
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
    listdir_ctx_t ctx = { L, lua_gettop(L), 0 };
    sdcard_list_dir(path, listdir_cb, &ctx);
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

void lua_bridge_register(lua_State *L) {
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

    // Push button constants into picocalc.input
    lua_getfield(L, -1, "input");
    lua_pushinteger(L, BTN_UP);    lua_setfield(L, -2, "BTN_UP");
    lua_pushinteger(L, BTN_DOWN);  lua_setfield(L, -2, "BTN_DOWN");
    lua_pushinteger(L, BTN_LEFT);  lua_setfield(L, -2, "BTN_LEFT");
    lua_pushinteger(L, BTN_RIGHT); lua_setfield(L, -2, "BTN_RIGHT");
    lua_pushinteger(L, BTN_ENTER); lua_setfield(L, -2, "BTN_ENTER");
    lua_pushinteger(L, BTN_ESC);   lua_setfield(L, -2, "BTN_ESC");
    lua_pushinteger(L, BTN_MENU);  lua_setfield(L, -2, "BTN_MENU");
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

    // Set as global
    lua_setglobal(L, "picocalc");
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

    display_draw_text(4, FB_HEIGHT - 12, "Press any key", COLOR_GRAY, COLOR_BLACK);
    display_flush();

    // Wait for a key press before returning
    while (!kbd_get_buttons()) {
        kbd_poll();
        sleep_ms(16);
    }
    lua_pop(L, 1);
}
