#include "lua_bridge_internal.h"
#include "../drivers/sdcard.h"
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
    
    luaL_Buffer buf;
    luaL_buffinit(L, &buf);
    
    lua_pushvalue(L, 2);
    lua_pushnil(L);
    
    luaL_addchar(&buf, '{');
    bool first = true;
    
    while (lua_next(L, -2) != 0) {
        if (!first) luaL_addchar(&buf, ',');
        first = false;
        
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *key = lua_tostring(L, -2);
            luaL_addchar(&buf, '"');
            luaL_addstring(&buf, key);
            luaL_addstring(&buf, "\":");
        } else {
            lua_pop(L, 1);
            continue;
        }
        
        int type = lua_type(L, -1);
        if (type == LUA_TSTRING) {
            const char *val = lua_tostring(L, -1);
            luaL_addchar(&buf, '"');
            luaL_addstring(&buf, val);
            luaL_addchar(&buf, '"');
        } else if (type == LUA_TNUMBER) {
            char num[32];
            snprintf(num, sizeof(num), "%g", lua_tonumber(L, -1));
            luaL_addstring(&buf, num);
        } else if (type == LUA_TBOOLEAN) {
            luaL_addstring(&buf, lua_toboolean(L, -1) ? "true" : "false");
        } else {
            luaL_addstring(&buf, "null");
        }
        
        lua_pop(L, 1);
    }
    
    luaL_addchar(&buf, '}');
    luaL_pushresult(&buf);
    
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
    
    lua_newtable(L);
    
    char *ptr = data;
    while (*ptr && *ptr != '{') ptr++;
    if (*ptr == '{') ptr++;
    
    while (*ptr && *ptr != '}') {
        while (*ptr && (*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == ',')) ptr++;
        if (*ptr == '}') break;
        
        if (*ptr == '"') {
            ptr++;
            char *key_start = ptr;
            while (*ptr && *ptr != '"') ptr++;
            size_t key_len = ptr - key_start;
            char *key = (char *)malloc(key_len + 1);
            memcpy(key, key_start, key_len);
            key[key_len] = '\0';
            if (*ptr == '"') ptr++;
            
            while (*ptr && *ptr != ':') ptr++;
            if (*ptr == ':') ptr++;
            while (*ptr && (*ptr == ' ' || *ptr == '\t')) ptr++;
            
            if (*ptr == '"') {
                ptr++;
                char *val_start = ptr;
                while (*ptr && *ptr != '"') ptr++;
                size_t val_len = ptr - val_start;
                char *val = (char *)malloc(val_len + 1);
                memcpy(val, val_start, val_len);
                val[val_len] = '\0';
                if (*ptr == '"') ptr++;
                
                lua_pushstring(L, val);
                lua_setfield(L, -2, key);
                free(val);
            } else if (*ptr >= '0' && *ptr <= '9' || *ptr == '-') {
                char *val_start = ptr;
                if (*ptr == '-') ptr++;
                while (*ptr && ((*ptr >= '0' && *ptr <= '9') || *ptr == '.')) ptr++;
                size_t val_len = ptr - val_start;
                char *val = (char *)malloc(val_len + 1);
                memcpy(val, val_start, val_len);
                val[val_len] = '\0';
                
                double num = atof(val);
                lua_pushnumber(L, num);
                lua_setfield(L, -2, key);
                free(val);
            } else if (strncmp(ptr, "true", 4) == 0) {
                ptr += 4;
                lua_pushboolean(L, 1);
                lua_setfield(L, -2, key);
            } else if (strncmp(ptr, "false", 5) == 0) {
                ptr += 5;
                lua_pushboolean(L, 0);
                lua_setfield(L, -2, key);
            }
            
            free(key);
        } else {
            ptr++;
        }
    }
    
    free(data);
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
