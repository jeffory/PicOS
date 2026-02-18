#include "launcher.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lua_bridge.h"

#include "pico/stdlib.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ── App discovery ─────────────────────────────────────────────────────────────

#define MAX_APPS 32

typedef struct {
    char name[64];         // Display name from app.json
    char path[128];        // Full path to app directory on SD card
    char description[128]; // Short description from app.json
    char version[16];
} app_entry_t;

static app_entry_t s_apps[MAX_APPS];
static int         s_app_count = 0;

// Tiny JSON parser - just enough to pull "name", "description", "version"
// from a simple flat JSON object. Not a full parser.
static bool json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;  // skip opening quote
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

static void on_app_dir(const sdcard_entry_t *entry, void *user) {
    (void)user;
    if (!entry->is_dir) return;
    if (entry->name[0] == '.') return;
    if (s_app_count >= MAX_APPS) return;

    // Check that main.lua exists
    char main_path[160];
    snprintf(main_path, sizeof(main_path), "/apps/%s/main.lua", entry->name);
    if (!sdcard_fexists(main_path)) return;

    app_entry_t *app = &s_apps[s_app_count];
    snprintf(app->path, sizeof(app->path), "/apps/%s", entry->name);

    // Try to load app.json for display name / description
    char json_path[160];
    snprintf(json_path, sizeof(json_path), "/apps/%s/app.json", entry->name);
    int json_len = 0;
    char *json = sdcard_read_file(json_path, &json_len);
    if (json) {
        if (!json_get_string(json, "name",        app->name,        sizeof(app->name)))
            strncpy(app->name, entry->name, sizeof(app->name));
        if (!json_get_string(json, "description", app->description, sizeof(app->description)))
            app->description[0] = '\0';
        if (!json_get_string(json, "version",     app->version,     sizeof(app->version)))
            strncpy(app->version, "1.0", sizeof(app->version));
        free(json);
    } else {
        strncpy(app->name, entry->name, sizeof(app->name));
        app->description[0] = '\0';
        strncpy(app->version, "?", sizeof(app->version));
    }

    s_app_count++;
}

static void scan_apps(void) {
    s_app_count = 0;
    sdcard_list_dir("/apps", on_app_dir, NULL);
}

// ── Launcher rendering ────────────────────────────────────────────────────────

#define ITEM_H       28
#define LIST_X        8
#define LIST_Y       32
#define LIST_VISIBLE  9

static int s_selected = 0;
static int s_scroll   = 0;

// Colour theme (easily remapped)
#define C_BG         COLOR_BLACK
#define C_HEADER_BG  RGB565(20, 20, 60)
#define C_SEL_BG     RGB565(40, 80, 160)
#define C_TEXT       COLOR_WHITE
#define C_TEXT_DIM   COLOR_GRAY
#define C_BATTERY_OK COLOR_GREEN
#define C_BATTERY_LO COLOR_RED
#define C_BORDER     RGB565(60, 60, 100)

static void draw_header(void) {
    display_fill_rect(0, 0, FB_WIDTH, 28, C_HEADER_BG);
    display_draw_text(8, 8, "PicoCalc OS", C_TEXT, C_HEADER_BG);

    // Battery indicator top-right
    int bat = kbd_get_battery_percent();
    if (bat >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Bat:%d%%", bat);
        uint16_t c = (bat > 20) ? C_BATTERY_OK : C_BATTERY_LO;
        display_draw_text(FB_WIDTH - strlen(buf) * 6 - 8, 8, buf, c, C_HEADER_BG);
    }

    display_fill_rect(0, 28, FB_WIDTH, 1, C_BORDER);
}

static void draw_footer(void) {
    display_fill_rect(0, FB_HEIGHT - 18, FB_WIDTH, 18, C_HEADER_BG);
    display_fill_rect(0, FB_HEIGHT - 18, FB_WIDTH, 1, C_BORDER);
    display_draw_text(8, FB_HEIGHT - 13, "Enter:Launch  Esc:Exit app  Sym:Menu", C_TEXT_DIM, C_HEADER_BG);
}

static void draw_launcher(void) {
    display_clear(C_BG);
    draw_header();
    draw_footer();

    if (s_app_count == 0) {
        display_draw_text(8, LIST_Y + 8, "No apps found.", C_TEXT_DIM, C_BG);
        display_draw_text(8, LIST_Y + 20, "Copy apps to /apps/ on SD card.", C_TEXT_DIM, C_BG);
        display_flush();
        return;
    }

    for (int i = 0; i < LIST_VISIBLE && (i + s_scroll) < s_app_count; i++) {
        int idx = i + s_scroll;
        int y = LIST_Y + i * ITEM_H;
        bool sel = (idx == s_selected);

        uint16_t bg = sel ? C_SEL_BG : C_BG;
        display_fill_rect(LIST_X - 4, y, FB_WIDTH - LIST_X * 2 + 8, ITEM_H - 2, bg);

        display_draw_text(LIST_X, y + 4, s_apps[idx].name, C_TEXT, bg);
        if (s_apps[idx].description[0]) {
            display_draw_text(LIST_X, y + 15, s_apps[idx].description, C_TEXT_DIM, bg);
        }
    }

    // Scrollbar
    if (s_app_count > LIST_VISIBLE) {
        int bar_h = (LIST_VISIBLE * ITEM_H) * LIST_VISIBLE / s_app_count;
        int bar_y = LIST_Y + (LIST_VISIBLE * ITEM_H) * s_scroll / s_app_count;
        display_fill_rect(FB_WIDTH - 6, LIST_Y, 4, LIST_VISIBLE * ITEM_H, C_BORDER);
        display_fill_rect(FB_WIDTH - 6, bar_y, 4, bar_h, C_TEXT);
    }

    display_flush();
}

// ── App runner ────────────────────────────────────────────────────────────────

static bool run_app(int idx) {
    if (idx < 0 || idx >= s_app_count) return false;

    // Read main.lua into memory
    char main_path[160];
    snprintf(main_path, sizeof(main_path), "%s/main.lua", s_apps[idx].path);

    int lua_len = 0;
    char *lua_src = sdcard_read_file(main_path, &lua_len);
    if (!lua_src) {
        display_clear(C_BG);
        display_draw_text(8, 8, "Failed to load app:", COLOR_RED, C_BG);
        display_draw_text(8, 20, main_path, C_TEXT, C_BG);
        display_flush();
        sleep_ms(2000);
        return false;
    }

    // Create a fresh Lua VM for this app
    lua_State *L = luaL_newstate();
    if (!L) { free(lua_src); return false; }

    lua_bridge_register(L);

    // Set app working directory as a global
    lua_pushstring(L, s_apps[idx].path);
    lua_setglobal(L, "APP_DIR");
    lua_pushstring(L, s_apps[idx].name);
    lua_setglobal(L, "APP_NAME");

    // Load and execute the app
    display_clear(C_BG);
    display_flush();

    int load_err = luaL_loadbuffer(L, lua_src, lua_len, s_apps[idx].name);
    free(lua_src);

    if (load_err != LUA_OK) {
        lua_bridge_show_error(L, "Load error:");
        lua_close(L);
        return false;
    }

    // pcall the chunk — the app runs inside this call
    // Apps that use a game loop should call picocalc.sys.sleep() each frame
    // or structure themselves with an update() function called from their own loop
    int run_err = lua_pcall(L, 0, 0, 0);
    if (run_err != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (!msg || !strstr(msg, "__picocalc_exit__")) {
            lua_bridge_show_error(L, "Runtime error:");
        } else {
            lua_pop(L, 1);  // discard sentinel
        }
    }

    lua_close(L);
    return true;
}

// ── Public interface ──────────────────────────────────────────────────────────

void launcher_run(void) {
    // Scan for apps on every launch so hot-swapping SD is supported
    scan_apps();
    draw_launcher();

    while (true) {
        kbd_poll();
        uint32_t pressed = kbd_get_buttons_pressed();

        bool dirty = false;

        if (pressed & BTN_UP) {
            if (s_selected > 0) {
                s_selected--;
                if (s_selected < s_scroll) s_scroll = s_selected;
                dirty = true;
            }
        }
        if (pressed & BTN_DOWN) {
            if (s_selected < s_app_count - 1) {
                s_selected++;
                if (s_selected >= s_scroll + LIST_VISIBLE)
                    s_scroll = s_selected - LIST_VISIBLE + 1;
                dirty = true;
            }
        }

        if (pressed & BTN_ENTER) {
            run_app(s_selected);
            // After app exits, re-scan and redraw
            scan_apps();
            s_selected = 0; s_scroll = 0;
            dirty = true;
        }

        if (dirty) draw_launcher();

        sleep_ms(16);   // ~60Hz polling
    }
}
