#include "lua_bridge_internal.h"
#include "../drivers/sound.h"

#define SAMPLE_USERDATA "sound_sample"
#define PLAYER_USERDATA "sound_player"

static sound_sample_t *check_sample(lua_State *L, int idx) {
    sound_sample_t **ud = luaL_checkudata(L, idx, SAMPLE_USERDATA);
    return *ud;
}

static sound_player_t *check_player(lua_State *L, int idx) {
    sound_player_t **ud = luaL_checkudata(L, idx, PLAYER_USERDATA);
    return *ud;
}

static int l_sound_sample_new(lua_State *L) {
    sound_sample_t *sample = sound_sample_create();
    if (!sample) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create sample");
        return 2;
    }

    const char *path = luaL_optstring(L, 1, NULL);
    if (path) {
        if (!sound_sample_load(sample, path)) {
            sound_sample_destroy(sample);
            lua_pushnil(L);
            lua_pushstring(L, "failed to load sample");
            return 2;
        }
    }

    sound_sample_t **ud = lua_newuserdata(L, sizeof(sound_sample_t *));
    *ud = sample;
    luaL_setmetatable(L, SAMPLE_USERDATA);
    return 1;
}

static int l_sound_sample_load(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    const char *path = luaL_checkstring(L, 2);

    if (sound_sample_load(sample, path)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "failed to load WAV file");
    return 2;
}

static int l_sound_sample_getLength(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    lua_pushinteger(L, sound_sample_get_length(sample));
    return 1;
}

static int l_sound_sample_getSampleRate(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    lua_pushinteger(L, sound_sample_get_sample_rate(sample));
    return 1;
}

static int l_sound_sample_gc(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    sound_sample_destroy(sample);
    return 0;
}

static int l_sound_sampleplayer_new(lua_State *L) {
    sound_sample_t *sample = NULL;

    if (lua_isuserdata(L, 1)) {
        sample = check_sample(L, 1);
    } else if (lua_isstring(L, 1)) {
        sample = sound_sample_create();
        if (!sound_sample_load(sample, lua_tostring(L, 1))) {
            sound_sample_destroy(sample);
            lua_pushnil(L);
            lua_pushstring(L, "failed to load sample");
            return 2;
        }
    }

    sound_player_t *player = sound_player_create();
    if (!player) {
        if (sample)
            sound_sample_destroy(sample);
        lua_pushnil(L);
        lua_pushstring(L, "failed to create player");
        return 2;
    }

    if (sample)
        sound_player_set_sample(player, sample);

    sound_player_t **ud = lua_newuserdata(L, sizeof(sound_player_t *));
    *ud = player;
    luaL_setmetatable(L, PLAYER_USERDATA);
    return 1;
}

static int l_sound_sampleplayer_setSample(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    sound_sample_t *sample = check_sample(L, 2);

    if (sound_player_set_sample(player, sample)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "failed to set sample");
    return 2;
}

static int l_sound_sampleplayer_play(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    uint8_t repeat = (uint8_t)luaL_optinteger(L, 2, 1);
    sound_player_play(player, repeat);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_sampleplayer_stop(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    sound_player_stop(player);
    return 0;
}

static int l_sound_sampleplayer_isPlaying(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_pushboolean(L, sound_player_is_playing(player));
    return 1;
}

static int l_sound_sampleplayer_setVolume(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    uint8_t vol = (uint8_t)luaL_checkinteger(L, 2);
    sound_player_set_volume(player, vol);
    return 0;
}

static int l_sound_sampleplayer_getVolume(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_pushinteger(L, player->volume);
    return 1;
}

static int l_sound_sampleplayer_gc(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    sound_player_destroy(player);
    return 0;
}

static int l_sound_getCurrentTime(lua_State *L) {
    lua_pushinteger(L, sound_get_current_time());
    return 1;
}

static int l_sound_resetTime(lua_State *L) {
    (void)L;
    sound_reset_time();
    return 0;
}

static const luaL_Reg sound_sample_methods[] = {
    {"load", l_sound_sample_load},
    {"getLength", l_sound_sample_getLength},
    {"getSampleRate", l_sound_sample_getSampleRate},
    {"__gc", l_sound_sample_gc},
    {NULL, NULL}
};

static const luaL_Reg sound_player_methods[] = {
    {"setSample", l_sound_sampleplayer_setSample},
    {"play", l_sound_sampleplayer_play},
    {"stop", l_sound_sampleplayer_stop},
    {"isPlaying", l_sound_sampleplayer_isPlaying},
    {"setVolume", l_sound_sampleplayer_setVolume},
    {"getVolume", l_sound_sampleplayer_getVolume},
    {"__gc", l_sound_sampleplayer_gc},
    {NULL, NULL}
};

static const luaL_Reg sound_funcs[] = {
    {"sample", l_sound_sample_new},
    {"sampleplayer", l_sound_sampleplayer_new},
    {"getCurrentTime", l_sound_getCurrentTime},
    {"resetTime", l_sound_resetTime},
    {NULL, NULL}
};

void lua_bridge_sound_init(lua_State *L) {
    luaL_newmetatable(L, SAMPLE_USERDATA);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, sound_sample_methods, 0);
    lua_pop(L, 1);

    luaL_newmetatable(L, PLAYER_USERDATA);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, sound_player_methods, 0);
    lua_pop(L, 1);

    register_subtable(L, "sound", sound_funcs);
}
