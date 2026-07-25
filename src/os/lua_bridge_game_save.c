#include "lua_bridge_internal.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_MAX_PATH 256
#define SAVE_MAX_DATA 65536

static int l_save_set(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    char path[SAVE_MAX_PATH];
    snprintf(path, sizeof(path), "/saves/%s.json", filename);

    // Uses the shared picocalc.json encoder. The previous implementation was a
    // flat inline loop that emitted `null` for any non-scalar value, so a table
    // with nested fields was silently written out as unrecoverable data loss.
    lua_json_encode_push(L, 2, 0);

    const char *json_str = lua_tostring(L, -1);
    size_t json_len = strlen(json_str);
    
    sdcard_mkdir("/saves");
    
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
    const char *filename = luaL_checkstring(L, 1);
    
    char path[SAVE_MAX_PATH];
    snprintf(path, sizeof(path), "/saves/%s.json", filename);
    
    int size = 0;
    char *data = sdcard_read_file(path, &size);
    if (!data) {
        lua_pushnil(L);
        return 1;
    }

    // Uses the shared picocalc.json decoder. The previous implementation was a
    // hand-rolled flat scanner that could only recover top-level scalars, so it
    // could not read back anything the (now fixed) encoder writes.
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

    // Guard the contract: callers index the result, so a save file holding a
    // bare scalar or array must not be handed back as one.
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }

    return 1;
}

static int l_save_exists(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    
    char path[SAVE_MAX_PATH];
    snprintf(path, sizeof(path), "/saves/%s.json", filename);
    
    lua_pushboolean(L, sdcard_fexists(path));
    return 1;
}

static int l_save_delete(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    
    char path[SAVE_MAX_PATH];
    snprintf(path, sizeof(path), "/saves/%s.json", filename);
    
    lua_pushboolean(L, sdcard_delete(path));
    return 1;
}

static void save_list_callback(const sdcard_entry_t *entry, void *user);

static int l_save_list(lua_State *L) {
    lua_newtable(L);
    
    int idx = 1;
    sdcard_list_dir("/saves", save_list_callback, &idx);
    
    return 1;
}

static void save_list_callback(const sdcard_entry_t *entry, void *user) {
    int *idx = (int *)user;
    if (!entry->is_dir) {
        char *ext = strrchr(entry->name, '.');
        if (ext && strcmp(ext, ".json") == 0) {
            *ext = '\0';
        }
    }
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
