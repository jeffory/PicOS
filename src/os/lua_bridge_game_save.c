#include "lua_bridge_internal.h"
#include "../drivers/sdcard.h"
#include "app_identity.h"
#include "fs_path.h"
#include "umm_malloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// picocalc.game.save: per-app JSON save slots.
//
// A slot lives at /data/<app_id>/saves/<name>.json, next to the app's config
// store; <app_id> comes from the C-owned app identity, never from a Lua
// global. Names are one path component of [A-Za-z0-9._-] (fs_name_valid), so
// no name can climb out of the directory or collide through truncation.
//
// Before this layout every app shared /saves/<name>.json. A slot still found
// there is moved into the first app that touches that name (get, exists, set
// or delete), so existing high scores survive the upgrade.

#define SAVE_MAX_NAME 128
// "/data/" + id (<80) + "/saves/" + name + ".json" + NUL
#define SAVE_MAX_PATH 256

// Validates the name at arg 1 and writes the slot's path to `path`; when the
// app's slot is missing but a legacy /saves/<name>.json exists, moves that
// file into place first. Returns false for a bad name or no running app.
static bool save_path(lua_State *L, char *path, size_t size) {
    size_t len = 0;
    const char *name = luaL_checklstring(L, 1, &len);
    const app_identity_t *me = app_identity_current();
    if (!me || !fs_name_valid(name, len, SAVE_MAX_NAME))
        return false;

    int n = snprintf(path, size, "%s/saves", me->data_dir);
    if (n < 0 || (size_t)n >= size)
        return false;
    if (!sdcard_fexists(path)) {
        sdcard_mkdir(me->data_dir);
        sdcard_mkdir(path);
    }
    n = snprintf(path, size, "%s/saves/%s.json", me->data_dir, name);
    if (n < 0 || (size_t)n >= size)
        return false;

    if (!sdcard_fexists(path)) {
        char legacy[SAVE_MAX_PATH];
        snprintf(legacy, sizeof(legacy), "/saves/%s.json", name);
        if (sdcard_fexists(legacy) && sdcard_rename(legacy, path))
            printf("[SAVE] migrated %s -> %s\n", legacy, path);
    }
    return true;
}

static int l_save_set(lua_State *L) {
    char path[SAVE_MAX_PATH];
    luaL_checktype(L, 2, LUA_TTABLE);
    if (!save_path(L, path, sizeof(path))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid save name");
        return 2;
    }

    // The shared picocalc.json encoder: nested tables, escaped strings,
    // integers as integers, floats in their shortest round-trip form.
    lua_json_encode_push(L, 2, 0);
    size_t json_len = 0;
    const char *json_str = lua_tolstring(L, -1, &json_len);

    sdfile_t file = sdcard_fopen(path, "w");
    if (!file) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "failed to open file for writing");
        return 2;
    }
    int written = sdcard_fwrite(file, json_str, json_len);
    sdcard_fclose(file);

    if (written != (int)json_len) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "failed to write data");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_save_get(lua_State *L) {
    char path[SAVE_MAX_PATH];
    if (!save_path(L, path, sizeof(path))) {
        lua_pushnil(L);
        return 1;
    }

    int size = 0;
    char *data = sdcard_read_file(path, &size);
    if (!data) {
        lua_pushnil(L);
        return 1;
    }

    const char *err = NULL;
    bool ok = lua_json_decode_push(L, data, (size_t)size, &err);
    umm_free(data);

    if (!ok) {
        // A corrupt or truncated save must not take the app down; nil reads the
        // same as "no save yet", which every caller already handles.
        printf("[SAVE] %s: %s\n", path, err ? err : "parse error");
        lua_pushnil(L);
        return 1;
    }
    // Callers index the result, so a bare scalar or array is not a save.
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    return 1;
}

static int l_save_exists(lua_State *L) {
    char path[SAVE_MAX_PATH];
    lua_pushboolean(L, save_path(L, path, sizeof(path)) && sdcard_fexists(path));
    return 1;
}

static int l_save_delete(lua_State *L) {
    char path[SAVE_MAX_PATH];
    lua_pushboolean(L, save_path(L, path, sizeof(path)) && sdcard_delete(path));
    return 1;
}

// l_save_list walks the directory twice: once to size the names, once to copy
// them into a Lua-owned buffer. No Lua call may run inside the sdcard_list_dir
// callback (an error there would unwind past the SD mutex and the open
// directory), and a Lua-owned buffer cannot leak if a later push fails.
typedef struct {
    char *buf;      // NULL on the sizing pass
    size_t len, cap;
} save_names_t;

static void save_list_callback(const sdcard_entry_t *entry, void *user) {
    save_names_t *s = (save_names_t *)user;
    if (entry->is_dir)
        return;
    size_t n = strlen(entry->name);
    if (n <= 5 || strcmp(entry->name + n - 5, ".json") != 0)
        return;
    n -= 5;
    if (!fs_name_valid(entry->name, n, SAVE_MAX_NAME))
        return;  // not a name get() could open
    if (s->buf) {
        if (s->len + n + 1 > s->cap)
            return;  // the directory grew between the passes
        memcpy(s->buf + s->len, entry->name, n);
        s->buf[s->len + n] = '\0';
    }
    s->len += n + 1;
}

static int l_save_list(lua_State *L) {
    lua_newtable(L);
    const app_identity_t *me = app_identity_current();
    if (!me)
        return 1;
    char path[SAVE_MAX_PATH];
    snprintf(path, sizeof(path), "%s/saves", me->data_dir);

    save_names_t s = {0};
    sdcard_list_dir(path, save_list_callback, &s);
    if (s.len == 0)
        return 1;
    s.cap = s.len;
    s.len = 0;
    s.buf = (char *)lua_newuserdatauv(L, s.cap, 0);
    sdcard_list_dir(path, save_list_callback, &s);

    int idx = 1;
    for (size_t off = 0; off < s.len; off += strlen(s.buf + off) + 1) {
        lua_pushstring(L, s.buf + off);
        lua_rawseti(L, -3, idx++);
    }
    lua_pop(L, 1);  // the name buffer
    return 1;
}

static const luaL_Reg l_save_lib[] = {
    {"set", l_save_set},
    {"get", l_save_get},
    {"exists", l_save_exists},
    {"delete", l_save_delete},
    {"list", l_save_list},
    {NULL, NULL}
};

void lua_bridge_game_save_init(lua_State *L) {
    lua_newtable(L);
    luaL_setfuncs(L, l_save_lib, 0);
}
