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
// Before this layout every app shared /saves/<name>.json. game.save never
// modifies or removes a file there. The first time an app reads (get/exists)
// a name it has no slot for, the legacy file is COPIED into its slot, so the
// original stays available to whichever app really owned it. A marker
// saves/.migrated-<name> records that the app has settled that name (copied
// it, or written or deleted its own slot), so a later delete is never undone
// by a fresh copy. Markers start with '.', which no slot name can, so list()
// never shows them.

#define SAVE_MAX_NAME 128
// "/data/" + id (<80) + "/saves/" + ".migrated-" + name + NUL
#define SAVE_MAX_PATH 256

typedef enum { SLOT_READ, SLOT_WRITE } slot_use_t;

// The name at arg 1, if valid for the running app; NULL otherwise. Pure.
static const char *slot_name(lua_State *L, const app_identity_t **me) {
    size_t len = 0;
    const char *name = luaL_checklstring(L, 1, &len);
    *me = app_identity_current();
    if (!*me || !fs_name_valid(name, len, SAVE_MAX_NAME))
        return NULL;
    return name;
}

static bool build(char *out, size_t size, const char *fmt,
                  const char *dir, const char *name) {
    int n = snprintf(out, size, fmt, dir, name);
    return n >= 0 && (size_t)n < size;
}

static void touch(const char *path) {
    sdfile_t f = sdcard_fopen(path, "w");
    if (f)
        sdcard_fclose(f);
}

// Creates the app's saves directory and writes the slot's path to `path`.
// For SLOT_READ, copies a legacy save into a missing, unsettled slot; for
// SLOT_WRITE (set/delete), settles the name without copying. The legacy file
// itself is only ever read.
static bool slot_prepare(const app_identity_t *me, const char *name,
                         slot_use_t use, char *path, size_t size) {
    // The marker is the longest path; if it fits, so do the others.
    char aux[SAVE_MAX_PATH];
    if (!build(path, size, "%s/saves/%s.json", me->data_dir, name) ||
        !build(aux, sizeof(aux), "%s/saves/.migrated-%s", me->data_dir, name))
        return false;

    snprintf(aux, sizeof(aux), "%s/saves", me->data_dir);
    if (!sdcard_fexists(aux)) {
        sdcard_mkdir(me->data_dir);
        sdcard_mkdir(aux);
    }

    char legacy[SAVE_MAX_PATH];
    snprintf(legacy, sizeof(legacy), "/saves/%s.json", name);
    snprintf(aux, sizeof(aux), "%s/saves/.migrated-%s", me->data_dir, name);
    if (sdcard_fexists(aux) || !sdcard_fexists(legacy))
        return true;  // settled, or nothing to migrate

    if (use == SLOT_WRITE) {
        touch(aux);
    } else if (!sdcard_fexists(path)) {
        if (sdcard_copy(legacy, path, NULL, NULL)) {
            touch(aux);
            printf("[SAVE] copied %s -> %s\n", legacy, path);
        }
    } else {
        touch(aux);  // the app already has its own slot
    }
    return true;
}

static int l_save_set(lua_State *L) {
    const app_identity_t *me;
    const char *name = slot_name(L, &me);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (!name) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid save name");
        return 2;
    }

    // Encode first: a table the encoder rejects (cycle, NaN, function) raises
    // before anything on the SD card changes. The shared picocalc.json
    // encoder handles nesting, escaping, integers and whole floats.
    lua_json_encode_push(L, 2, 0);
    size_t json_len = 0;
    const char *json_str = lua_tolstring(L, -1, &json_len);

    char path[SAVE_MAX_PATH];
    if (!slot_prepare(me, name, SLOT_WRITE, path, sizeof(path))) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid save name");
        return 2;
    }
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
    const app_identity_t *me;
    const char *name = slot_name(L, &me);
    char path[SAVE_MAX_PATH];
    if (!name || !slot_prepare(me, name, SLOT_READ, path, sizeof(path))) {
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
    const app_identity_t *me;
    const char *name = slot_name(L, &me);
    char path[SAVE_MAX_PATH];
    lua_pushboolean(L, name &&
                           slot_prepare(me, name, SLOT_READ, path, sizeof(path)) &&
                           sdcard_fexists(path));
    return 1;
}

static int l_save_delete(lua_State *L) {
    const app_identity_t *me;
    const char *name = slot_name(L, &me);
    char path[SAVE_MAX_PATH];
    lua_pushboolean(L, name &&
                           slot_prepare(me, name, SLOT_WRITE, path, sizeof(path)) &&
                           sdcard_delete(path));
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
