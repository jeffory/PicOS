#include "lua_bridge_internal.h"
#include "../drivers/sound.h"
#include "../drivers/fileplayer.h"
#include "../drivers/mp3_player.h"

#define SAMPLE_USERDATA "sound_sample"
#define PLAYER_USERDATA "sound_player"
#define FILEPLAYER_USERDATA "fileplayer"
#define MP3PLAYER_USERDATA "mp3player"

static sound_sample_t *check_sample(lua_State *L, int idx) {
    sound_sample_t **ud = luaL_checkudata(L, idx, SAMPLE_USERDATA);
    return *ud;
}

static sound_player_t *check_player(lua_State *L, int idx) {
    sound_player_t **ud = luaL_checkudata(L, idx, PLAYER_USERDATA);
    return *ud;
}

static fileplayer_t *check_fileplayer(lua_State *L, int idx) {
    fileplayer_t **ud = luaL_checkudata(L, idx, FILEPLAYER_USERDATA);
    return *ud;
}

static mp3_player_t *check_mp3player(lua_State *L, int idx) {
    mp3_player_t **ud = luaL_checkudata(L, idx, MP3PLAYER_USERDATA);
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
    lua_pushinteger(L, sound_player_get_volume(player));
    return 1;
}

static int l_sound_sampleplayer_gc(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    sound_player_destroy(player);
    return 0;
}

static int l_sound_fileplayer_new(lua_State *L) {
    size_t buffer_size = luaL_optinteger(L, 1, 8192);
    (void)buffer_size;

    fileplayer_t *player = fileplayer_create();
    if (!player) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create fileplayer");
        return 2;
    }

    fileplayer_t **ud = lua_newuserdata(L, sizeof(fileplayer_t *));
    *ud = player;
    luaL_setmetatable(L, FILEPLAYER_USERDATA);
    return 1;
}

static int l_sound_fileplayer_load(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    const char *path = luaL_checkstring(L, 2);

    if (fileplayer_load(player, path)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "failed to load file");
    return 2;
}

static int l_sound_fileplayer_play(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint8_t repeat = (uint8_t)luaL_optinteger(L, 2, 1);
    if (fileplayer_play(player, repeat)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "failed to play");
    return 2;
}

static int l_sound_fileplayer_stop(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    fileplayer_stop(player);
    return 0;
}

static int l_sound_fileplayer_pause(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    fileplayer_pause(player);
    return 0;
}

static int l_sound_fileplayer_isPlaying(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushboolean(L, fileplayer_is_playing(player));
    return 1;
}

static int l_sound_fileplayer_getLength(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushinteger(L, fileplayer_get_length(player));
    return 1;
}

static int l_sound_fileplayer_getOffset(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushinteger(L, fileplayer_get_offset(player));
    return 1;
}

static int l_sound_fileplayer_setOffset(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint32_t seconds = (uint32_t)luaL_checkinteger(L, 2);
    fileplayer_set_offset(player, seconds);
    return 0;
}

static int l_sound_fileplayer_setVolume(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint8_t left = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t right = (uint8_t)luaL_optinteger(L, 3, 0);
    fileplayer_set_volume(player, left, right);
    return 0;
}

static int l_sound_fileplayer_getVolume(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint8_t left, right;
    fileplayer_get_volume(player, &left, &right);
    lua_pushinteger(L, left);
    lua_pushinteger(L, right);
    return 2;
}

static int l_sound_fileplayer_setLoopRange(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint32_t start = (uint32_t)luaL_optinteger(L, 2, 0);
    uint32_t end = (uint32_t)luaL_optinteger(L, 3, 0);
    fileplayer_set_loop_range(player, start, end);
    return 0;
}

static int l_sound_fileplayer_gc(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    fileplayer_destroy(player);
    return 0;
}

static int l_sound_mp3player_new(lua_State *L) {
    mp3_player_t *player = mp3_player_create();
    if (!player) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create mp3 player");
        return 2;
    }

    mp3_player_t **ud = lua_newuserdata(L, sizeof(mp3_player_t *));
    *ud = player;
    luaL_setmetatable(L, MP3PLAYER_USERDATA);
    return 1;
}

static int l_sound_mp3player_load(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    const char *path = luaL_checkstring(L, 2);

    if (mp3_player_load(player, path)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "failed to load MP3 file");
    return 2;
}

static int l_sound_mp3player_play(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    uint8_t repeat = (uint8_t)luaL_optinteger(L, 2, 1);
    mp3_player_set_loop(player, repeat == 0);
    if (mp3_player_play(player, repeat)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "failed to play");
    return 2;
}

static int l_sound_mp3player_stop(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    mp3_player_stop(player);
    return 0;
}

static int l_sound_mp3player_pause(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    mp3_player_pause(player);
    return 0;
}

static int l_sound_mp3player_resume(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    mp3_player_resume(player);
    return 0;
}

static int l_sound_mp3player_isPlaying(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushboolean(L, mp3_player_is_playing(player));
    return 1;
}

static int l_sound_mp3player_getPosition(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushinteger(L, mp3_player_get_position(player));
    return 1;
}

static int l_sound_mp3player_getLength(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushinteger(L, mp3_player_get_length(player));
    return 1;
}

static int l_sound_mp3player_setVolume(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    uint8_t vol = (uint8_t)luaL_checkinteger(L, 2);
    mp3_player_set_volume(player, vol);
    return 0;
}

static int l_sound_mp3player_getVolume(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushinteger(L, mp3_player_get_volume(player));
    return 1;
}

static int l_sound_mp3player_setLoop(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    bool loop = lua_toboolean(L, 2);
    mp3_player_set_loop(player, loop);
    return 0;
}

static int l_sound_mp3player_gc(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    mp3_player_destroy(player);
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

static const luaL_Reg sound_fileplayer_methods[] = {
    {"load", l_sound_fileplayer_load},
    {"play", l_sound_fileplayer_play},
    {"stop", l_sound_fileplayer_stop},
    {"pause", l_sound_fileplayer_pause},
    {"isPlaying", l_sound_fileplayer_isPlaying},
    {"getLength", l_sound_fileplayer_getLength},
    {"getOffset", l_sound_fileplayer_getOffset},
    {"setOffset", l_sound_fileplayer_setOffset},
    {"setVolume", l_sound_fileplayer_setVolume},
    {"getVolume", l_sound_fileplayer_getVolume},
    {"setLoopRange", l_sound_fileplayer_setLoopRange},
    {"__gc", l_sound_fileplayer_gc},
    {NULL, NULL}
};

static const luaL_Reg sound_mp3player_methods[] = {
    {"load", l_sound_mp3player_load},
    {"play", l_sound_mp3player_play},
    {"stop", l_sound_mp3player_stop},
    {"pause", l_sound_mp3player_pause},
    {"resume", l_sound_mp3player_resume},
    {"isPlaying", l_sound_mp3player_isPlaying},
    {"getPosition", l_sound_mp3player_getPosition},
    {"getLength", l_sound_mp3player_getLength},
    {"setVolume", l_sound_mp3player_setVolume},
    {"getVolume", l_sound_mp3player_getVolume},
    {"setLoop", l_sound_mp3player_setLoop},
    {"__gc", l_sound_mp3player_gc},
    {NULL, NULL}
};

static const luaL_Reg sound_funcs[] = {
    {"sample", l_sound_sample_new},
    {"sampleplayer", l_sound_sampleplayer_new},
    {"fileplayer", l_sound_fileplayer_new},
    {"mp3player", l_sound_mp3player_new},
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

    luaL_newmetatable(L, FILEPLAYER_USERDATA);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, sound_fileplayer_methods, 0);
    lua_pop(L, 1);

    luaL_newmetatable(L, MP3PLAYER_USERDATA);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, sound_mp3player_methods, 0);
    lua_pop(L, 1);

    printf("[SOUND] Initializing fileplayer...\n");
    fileplayer_init();
    printf("[SOUND] fileplayer_init done\n");
    
    printf("[SOUND] Initializing mp3_player...\n");
    mp3_player_init();
    printf("[SOUND] mp3_player_init done\n");

    register_subtable(L, "sound", sound_funcs);
}
