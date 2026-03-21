#include "lua_bridge_internal.h"
#include "../drivers/sound.h"
#include "../drivers/fileplayer.h"
#include "../drivers/mp3_player.h"
// Note: sound.h, fileplayer.h, mp3_player.h are kept for functions not in
// g_api.soundplayer (e.g. sound_sample_new_blank, sound_player_set_rate,
// fileplayer_set_finish_callback, mp3_player_get_position, init/reset calls).

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
    if (lua_isnumber(L, 1)) {
        float seconds = (float)lua_tonumber(L, 1);
        uint32_t sample_rate = 44100;
        uint8_t bits = 16;
        uint8_t channels = 1;

        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "sampleRate");
            if (!lua_isnil(L, -1)) sample_rate = (uint32_t)lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, 2, "bits");
            if (!lua_isnil(L, -1)) bits = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, 2, "channels");
            if (!lua_isnil(L, -1)) channels = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }

        sound_sample_t *sample = sound_sample_new_blank(seconds, sample_rate, bits, channels);
        if (!sample) {
            lua_pushnil(L);
            lua_pushstring(L, "failed to allocate sample buffer");
            return 2;
        }

        sound_sample_t **ud = lua_newuserdata(L, sizeof(sound_sample_t *));
        *ud = sample;
        luaL_setmetatable(L, SAMPLE_USERDATA);
        return 1;
    }

    const char *path = luaL_optstring(L, 1, NULL);
    if (path) {
        sound_sample_t *sample = (sound_sample_t *)g_api.soundplayer->sampleLoad(path);
        if (!sample) {
            lua_pushnil(L);
            lua_pushstring(L, "failed to load sample");
            return 2;
        }
        sound_sample_t **ud = lua_newuserdata(L, sizeof(sound_sample_t *));
        *ud = sample;
        luaL_setmetatable(L, SAMPLE_USERDATA);
        return 1;
    }

    // No path: create an empty (unloaded) sample
    sound_sample_t *sample = sound_sample_create();
    if (!sample) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create sample");
        return 2;
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

static int l_sound_sample_getFormat(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    lua_newtable(L);
    lua_pushinteger(L, sample->bits_per_sample);
    lua_setfield(L, -2, "bits");
    lua_pushinteger(L, sample->channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, sample->sample_rate);
    lua_setfield(L, -2, "sampleRate");
    return 1;
}

static int l_sound_sample_decompress(lua_State *L) {
    check_sample(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

static int l_sound_sample_getSubsample(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    uint32_t start = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t end = (uint32_t)luaL_checkinteger(L, 3);

    sound_sample_t *sub = sound_sample_get_subsample(sample, start, end);
    if (!sub) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create subsample");
        return 2;
    }

    sound_sample_t **ud = lua_newuserdata(L, sizeof(sound_sample_t *));
    *ud = sub;
    luaL_setmetatable(L, SAMPLE_USERDATA);
    return 1;
}

static int l_sound_sample_gc(lua_State *L) {
    sound_sample_t *sample = check_sample(L, 1);
    g_api.soundplayer->sampleFree(sample);
    return 0;
}

static int l_sound_sampleplayer_new(lua_State *L) {
    sound_sample_t *sample = NULL;

    if (lua_isuserdata(L, 1)) {
        sample = check_sample(L, 1);
    } else if (lua_isstring(L, 1)) {
        sample = (sound_sample_t *)g_api.soundplayer->sampleLoad(lua_tostring(L, 1));
        if (!sample) {
            lua_pushnil(L);
            lua_pushstring(L, "failed to load sample");
            return 2;
        }
    }

    sound_player_t *player = (sound_player_t *)g_api.soundplayer->playerNew();
    if (!player) {
        if (sample)
            g_api.soundplayer->sampleFree(sample);
        lua_pushnil(L);
        lua_pushstring(L, "failed to create player");
        return 2;
    }

    if (sample)
        g_api.soundplayer->playerSetSample(player, sample);

    sound_player_t **ud = lua_newuserdata(L, sizeof(sound_player_t *));
    *ud = player;
    luaL_setmetatable(L, PLAYER_USERDATA);
    return 1;
}

static int l_sound_sampleplayer_setSample(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    sound_sample_t *sample = check_sample(L, 2);
    g_api.soundplayer->playerSetSample(player, sample);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_sampleplayer_play(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    uint8_t repeat = (uint8_t)luaL_optinteger(L, 2, 1);
    g_api.soundplayer->playerPlay(player, repeat);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_sampleplayer_stop(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    g_api.soundplayer->playerStop(player);
    return 0;
}

static int l_sound_sampleplayer_isPlaying(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_pushboolean(L, g_api.soundplayer->playerIsPlaying(player));
    return 1;
}

static int l_sound_sampleplayer_setVolume(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    uint8_t vol = (uint8_t)luaL_checkinteger(L, 2);
    g_api.soundplayer->playerSetVolume(player, vol);
    return 0;
}

static int l_sound_sampleplayer_getVolume(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_pushinteger(L, g_api.soundplayer->playerGetVolume(player));
    return 1;
}

static int l_sound_sampleplayer_setPaused(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    player->paused = lua_toboolean(L, 2);
    return 0;
}

static int l_sound_sampleplayer_getLength(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    if (player->sample) {
        lua_pushinteger(L, sound_sample_get_length(player->sample));
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

static int l_sound_sampleplayer_setOffset(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_Number seconds = luaL_checknumber(L, 2);
    if (player->sample && player->sample->loaded) {
        uint32_t bytes_per_sample = player->sample->bits_per_sample / 8;
        uint32_t bytes_per_frame = bytes_per_sample * player->sample->channels;
        uint32_t frame = (uint32_t)(seconds * player->sample->sample_rate);
        uint32_t byte_offset = frame * bytes_per_frame;
        if (byte_offset > player->sample->length)
            byte_offset = player->sample->length;
        player->position = byte_offset;
    }
    return 0;
}

static int l_sound_sampleplayer_getOffset(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    if (player->sample && player->sample->loaded && player->sample->sample_rate > 0) {
        uint32_t bytes_per_sample = player->sample->bits_per_sample / 8;
        uint32_t bytes_per_frame = bytes_per_sample * player->sample->channels;
        lua_Number seconds = (lua_Number)(player->position / bytes_per_frame)
                             / (lua_Number)player->sample->sample_rate;
        lua_pushnumber(L, seconds);
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int l_sound_sampleplayer_getSample(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    if (player->sample) {
        lua_pushlightuserdata(L, player->sample);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_sound_sampleplayer_setPlayRange(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    uint32_t start = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t end = (uint32_t)luaL_checkinteger(L, 3);
    sound_player_set_play_range(player, start, end);
    return 0;
}

static int l_sound_sampleplayer_setRate(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    float rate = (float)luaL_checknumber(L, 2);
    sound_player_set_rate(player, rate);
    return 0;
}

static int l_sound_sampleplayer_getRate(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    lua_pushnumber(L, sound_player_get_rate(player));
    return 1;
}

static int l_sound_sampleplayer_gc(lua_State *L) {
    sound_player_t *player = check_player(L, 1);
    g_api.soundplayer->playerFree(player);
    return 0;
}

static int l_sound_fileplayer_new(lua_State *L) {
    size_t buffer_size = luaL_optinteger(L, 1, 8192);
    (void)buffer_size;

    fileplayer_t *player = (fileplayer_t *)g_api.soundplayer->filePlayerNew();
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
    g_api.soundplayer->filePlayerLoad(player, path);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_fileplayer_play(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint8_t repeat = (uint8_t)luaL_optinteger(L, 2, 1);
    g_api.soundplayer->filePlayerPlay(player, repeat);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_fileplayer_stop(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    g_api.soundplayer->filePlayerStop(player);
    return 0;
}

static int l_sound_fileplayer_pause(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    g_api.soundplayer->filePlayerPause(player);
    return 0;
}

static int l_sound_fileplayer_isPlaying(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushboolean(L, g_api.soundplayer->filePlayerIsPlaying(player));
    return 1;
}

static int l_sound_fileplayer_getLength(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushinteger(L, fileplayer_get_length(player));
    return 1;
}

static int l_sound_fileplayer_getOffset(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    lua_pushinteger(L, g_api.soundplayer->filePlayerGetOffset(player));
    return 1;
}

static int l_sound_fileplayer_setOffset(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint32_t seconds = (uint32_t)luaL_checkinteger(L, 2);
    g_api.soundplayer->filePlayerSetOffset(player, seconds);
    return 0;
}

static int l_sound_fileplayer_setVolume(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint8_t left = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t right = (uint8_t)luaL_optinteger(L, 3, left);
    // API takes a single volume value; use left channel value (sets both channels)
    (void)right;
    g_api.soundplayer->filePlayerSetVolume(player, left);
    return 0;
}

static int l_sound_fileplayer_getVolume(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint8_t vol = g_api.soundplayer->filePlayerGetVolume(player);
    lua_pushinteger(L, vol);
    lua_pushinteger(L, vol);
    return 2;
}

static int l_sound_fileplayer_setLoopRange(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    uint32_t start = (uint32_t)luaL_optinteger(L, 2, 0);
    uint32_t end = (uint32_t)luaL_optinteger(L, 3, 0);
    fileplayer_set_loop_range(player, start, end);
    return 0;
}

static int l_sound_fileplayer_resume(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    g_api.soundplayer->filePlayerResume(player);
    return 0;
}

// Finish callback: stores a Lua function ref and fires it when playback ends.
// The C callback is invoked from fileplayer_update() on Core 1, but we defer
// to Core 0 by checking in the Lua hook. For simplicity, we fire immediately
// since fileplayer_update sets state to IDLE which isPlaying() already reflects.
typedef struct {
    lua_State *L;
    int ref;
} fileplayer_lua_cb_t;

#define MAX_FILEPLAYER_CBS 2
static fileplayer_lua_cb_t s_fp_cbs[MAX_FILEPLAYER_CBS];

static int fileplayer_lua_finish_trampoline(void *arg) {
    // This runs on Core 1 — we can't call Lua here.
    // The finish callback in the C driver just sets state to IDLE;
    // apps should poll isPlaying() or we could add a pending flag.
    // For now, just return 0 to indicate success.
    (void)arg;
    return 0;
}

static int l_sound_fileplayer_setFinishCallback(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // Find or allocate a callback slot for this player
    fileplayer_lua_cb_t *cb = NULL;
    for (int i = 0; i < MAX_FILEPLAYER_CBS; i++) {
        if (s_fp_cbs[i].ref != 0 && player->finish_callback_arg == &s_fp_cbs[i]) {
            // Reuse existing slot for this player
            luaL_unref(L, LUA_REGISTRYINDEX, s_fp_cbs[i].ref);
            cb = &s_fp_cbs[i];
            break;
        }
    }
    if (!cb) {
        for (int i = 0; i < MAX_FILEPLAYER_CBS; i++) {
            if (s_fp_cbs[i].ref == 0) {
                cb = &s_fp_cbs[i];
                break;
            }
        }
    }
    if (!cb)
        return luaL_error(L, "too many fileplayer finish callbacks");

    lua_pushvalue(L, 2);
    cb->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    cb->L = L;

    fileplayer_set_finish_callback(player, fileplayer_lua_finish_trampoline, cb);
    return 0;
}

static int l_sound_fileplayer_didUnderrun(lua_State *L) {
    (void)check_fileplayer(L, 1);
    lua_pushboolean(L, fileplayer_did_underrun());
    return 1;
}

static int l_sound_fileplayer_setStopOnUnderrun(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    bool flag = lua_toboolean(L, 2);
    fileplayer_set_stop_on_underrun(player, flag);
    return 0;
}

static int l_sound_fileplayer_gc(lua_State *L) {
    fileplayer_t *player = check_fileplayer(L, 1);
    g_api.soundplayer->filePlayerFree(player);
    return 0;
}

static int l_sound_mp3player_new(lua_State *L) {
    mp3_player_t *player = (mp3_player_t *)g_api.soundplayer->mp3PlayerNew();
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
    g_api.soundplayer->mp3PlayerLoad(player, path);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_mp3player_play(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    uint8_t repeat = (uint8_t)luaL_optinteger(L, 2, 1);
    g_api.soundplayer->mp3PlayerSetLoop(player, repeat == 0);
    g_api.soundplayer->mp3PlayerPlay(player, repeat);
    lua_pushboolean(L, true);
    return 1;
}

static int l_sound_mp3player_stop(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    g_api.soundplayer->mp3PlayerStop(player);
    return 0;
}

static int l_sound_mp3player_pause(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    g_api.soundplayer->mp3PlayerPause(player);
    return 0;
}

static int l_sound_mp3player_resume(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    g_api.soundplayer->mp3PlayerResume(player);
    return 0;
}

static int l_sound_mp3player_isPlaying(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushboolean(L, g_api.soundplayer->mp3PlayerIsPlaying(player));
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
    g_api.soundplayer->mp3PlayerSetVolume(player, vol);
    return 0;
}

static int l_sound_mp3player_getVolume(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushinteger(L, g_api.soundplayer->mp3PlayerGetVolume(player));
    return 1;
}

static int l_sound_mp3player_getSampleRate(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    lua_pushinteger(L, mp3_player_get_sample_rate(player));
    return 1;
}

static int l_sound_mp3player_setLoop(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    bool loop = lua_toboolean(L, 2);
    g_api.soundplayer->mp3PlayerSetLoop(player, loop);
    return 0;
}

static int l_sound_mp3player_gc(lua_State *L) {
    mp3_player_t *player = check_mp3player(L, 1);
    g_api.soundplayer->mp3PlayerFree(player);
    return 0;
}

static int l_sound_playingSources(lua_State *L) {
    lua_pushinteger(L, sound_get_playing_source_count());
    return 1;
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
    {"getFormat", l_sound_sample_getFormat},
    {"decompress", l_sound_sample_decompress},
    {"getSubsample", l_sound_sample_getSubsample},
    {"__gc", l_sound_sample_gc},
    {NULL, NULL}
};

static const luaL_Reg sound_player_methods[] = {
    {"setSample", l_sound_sampleplayer_setSample},
    {"getSample", l_sound_sampleplayer_getSample},
    {"play", l_sound_sampleplayer_play},
    {"stop", l_sound_sampleplayer_stop},
    {"isPlaying", l_sound_sampleplayer_isPlaying},
    {"setPaused", l_sound_sampleplayer_setPaused},
    {"getLength", l_sound_sampleplayer_getLength},
    {"setOffset", l_sound_sampleplayer_setOffset},
    {"getOffset", l_sound_sampleplayer_getOffset},
    {"setVolume", l_sound_sampleplayer_setVolume},
    {"getVolume", l_sound_sampleplayer_getVolume},
    {"setPlayRange", l_sound_sampleplayer_setPlayRange},
    {"setRate", l_sound_sampleplayer_setRate},
    {"getRate", l_sound_sampleplayer_getRate},
    {"__gc", l_sound_sampleplayer_gc},
    {NULL, NULL}
};

static const luaL_Reg sound_fileplayer_methods[] = {
    {"load", l_sound_fileplayer_load},
    {"play", l_sound_fileplayer_play},
    {"stop", l_sound_fileplayer_stop},
    {"pause", l_sound_fileplayer_pause},
    {"resume", l_sound_fileplayer_resume},
    {"isPlaying", l_sound_fileplayer_isPlaying},
    {"getLength", l_sound_fileplayer_getLength},
    {"getOffset", l_sound_fileplayer_getOffset},
    {"setOffset", l_sound_fileplayer_setOffset},
    {"setVolume", l_sound_fileplayer_setVolume},
    {"getVolume", l_sound_fileplayer_getVolume},
    {"setLoopRange", l_sound_fileplayer_setLoopRange},
    {"didUnderrun", l_sound_fileplayer_didUnderrun},
    {"setFinishCallback", l_sound_fileplayer_setFinishCallback},
    {"setStopOnUnderrun", l_sound_fileplayer_setStopOnUnderrun},
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
    {"getSampleRate", l_sound_mp3player_getSampleRate},
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
    {"playingSources", l_sound_playingSources},
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

    // Reset audio state from any previous app (stop timers, close files, clear players)
    sound_init();
    fileplayer_reset();
    mp3_player_reset();

    // Clear Lua callback refs from previous app
    memset(s_fp_cbs, 0, sizeof(s_fp_cbs));

    printf("[SOUND] Initializing fileplayer...\n");
    fileplayer_init();
    printf("[SOUND] fileplayer_init done\n");

    printf("[SOUND] Initializing mp3_player...\n");
    mp3_player_init();
    printf("[SOUND] mp3_player_init done\n");

    register_subtable(L, "sound", sound_funcs);
}
