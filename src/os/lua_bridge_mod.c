#include "lua_bridge_internal.h"
#include "../drivers/mod_player.h"

#define MODPLAYER_USERDATA "modplayer"

static mod_player_t *check_modplayer(lua_State *L, int idx) {
    mod_player_t **ud = luaL_checkudata(L, idx, MODPLAYER_USERDATA);
    return *ud;
}

static int l_mod_create(lua_State *L) {
    mod_player_t *p = mod_player_create();
    if (!p) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create MOD player");
        return 2;
    }
    mod_player_t **ud = lua_newuserdata(L, sizeof(mod_player_t *));
    *ud = p;
    luaL_setmetatable(L, MODPLAYER_USERDATA);
    return 1;
}

static int l_mod_load(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    const char *path = luaL_checkstring(L, 2);
    lua_pushboolean(L, mod_player_load(p, path));
    return 1;
}

static int l_mod_play(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    bool loop = false;
    if (lua_isboolean(L, 2)) loop = lua_toboolean(L, 2);
    mod_player_play(p, loop);
    return 0;
}

static int l_mod_stop(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    mod_player_stop(p);
    return 0;
}

static int l_mod_pause(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    mod_player_pause(p);
    return 0;
}

static int l_mod_resume(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    mod_player_resume(p);
    return 0;
}

static int l_mod_is_playing(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    lua_pushboolean(L, mod_player_is_playing(p));
    return 1;
}

static int l_mod_set_volume(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    int vol = (int)luaL_checkinteger(L, 2);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    mod_player_set_volume(p, (uint8_t)vol);
    return 0;
}

static int l_mod_get_volume(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    lua_pushinteger(L, mod_player_get_volume(p));
    return 1;
}

static int l_mod_set_loop(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    mod_player_set_loop(p, lua_toboolean(L, 2));
    return 0;
}

static int l_mod_gc(lua_State *L) {
    mod_player_t *p = check_modplayer(L, 1);
    mod_player_destroy(p);
    return 0;
}

static const luaL_Reg modplayer_methods[] = {
    {"load",       l_mod_load},
    {"play",       l_mod_play},
    {"stop",       l_mod_stop},
    {"pause",      l_mod_pause},
    {"resume",     l_mod_resume},
    {"isPlaying",  l_mod_is_playing},
    {"setVolume",  l_mod_set_volume},
    {"getVolume",  l_mod_get_volume},
    {"setLoop",    l_mod_set_loop},
    {"__gc",       l_mod_gc},
    {NULL, NULL}
};

static const luaL_Reg modplayer_funcs[] = {
    {"create", l_mod_create},
    {NULL, NULL}
};

void lua_bridge_mod_init(lua_State *L) {
    // Create the modplayer metatable
    luaL_newmetatable(L, MODPLAYER_USERDATA);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, modplayer_methods, 0);
    lua_pop(L, 1);

    // Register picocalc.modplayer subtable
    register_subtable(L, "modplayer", modplayer_funcs);
}
