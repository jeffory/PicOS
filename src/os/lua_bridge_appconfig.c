#include "lua_bridge_internal.h"
#include "appconfig.h"

static int l_config_get(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    const char *fallback = lua_isnoneornil(L, 2) ? NULL : luaL_optstring(L, 2, NULL);
    
    if (!appconfig_get_app_id()) {
        lua_getglobal(L, "APP_ID");
        const char *app_id = lua_tostring(L, -1);
        lua_pop(L, 1);
        
        if (app_id && app_id[0]) {
            appconfig_load(app_id);
        } else {
            lua_pushnil(L);
            return 1;
        }
    }
    
    const char *val = appconfig_get(key, fallback);
    if (val)
        lua_pushstring(L, val);
    else
        lua_pushnil(L);
    return 1;
}

static int l_config_set(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    const char *val = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
    
    if (!appconfig_get_app_id()) {
        lua_getglobal(L, "APP_ID");
        const char *app_id = lua_tostring(L, -1);
        lua_pop(L, 1);
        
        if (!app_id || !app_id[0]) {
            return 0;
        }
        appconfig_load(app_id);
    }
    
    if (val)
        appconfig_set(key, val);
    else
        appconfig_set(key, "");
    return 0;
}

static int l_config_save(lua_State *L) {
    (void)L;
    bool ok = appconfig_save();
    lua_pushboolean(L, ok);
    return 1;
}

static int l_config_clear(lua_State *L) {
    (void)L;
    appconfig_clear();
    return 0;
}

static int l_config_reset(lua_State *L) {
    (void)L;
    lua_pushboolean(L, appconfig_reset());
    return 1;
}

static int l_config_load(lua_State *L) {
    (void)L;
    lua_getglobal(L, "APP_ID");
    const char *app_id = lua_tostring(L, -1);
    lua_pop(L, 1);
    
    if (!app_id || !app_id[0]) {
        lua_pushboolean(L, false);
        return 1;
    }
    
    lua_pushboolean(L, appconfig_load(app_id));
    return 1;
}

static const luaL_Reg l_config_lib[] = {
    {"get",    l_config_get},
    {"set",    l_config_set},
    {"save",   l_config_save},
    {"load",   l_config_load},
    {"clear",  l_config_clear},
    {"reset",  l_config_reset},
    {NULL, NULL}
};

void lua_bridge_appconfig_init(lua_State *L) {
    register_subtable(L, "config", l_config_lib);
}
