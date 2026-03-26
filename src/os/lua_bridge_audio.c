#include "lua_bridge_internal.h"
#include "../drivers/audio.h"

// ── picocalc.audio.* ───────────────────────────────────────────────────────────

static int l_audio_playTone(lua_State *L) {
  uint32_t freq = (uint32_t)luaL_checkinteger(L, 1);
  uint32_t duration = (uint32_t)luaL_optinteger(L, 2, 0);
  audio_play_tone(freq, duration);
  return 0;
}

static int l_audio_stopTone(lua_State *L) {
  (void)L;
  audio_stop_tone();
  return 0;
}

static int l_audio_setVolume(lua_State *L) {
  uint8_t vol = (uint8_t)luaL_checkinteger(L, 1);
  audio_set_volume(vol);
  return 0;
}

static int l_audio_startStream(lua_State *L) {
  uint32_t rate = (uint32_t)luaL_checkinteger(L, 1);
  audio_start_stream(rate);
  return 0;
}

static int l_audio_stopStream(lua_State *L) {
  (void)L;
  audio_stop_stream();
  return 0;
}

static int l_audio_pushSamples(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  int n = (int)luaL_len(L, 1);
  int16_t buf[512];
  int count = (n > 512) ? 512 : n;
  for (int i = 0; i < count; i++) {
    lua_rawgeti(L, 1, i + 1);
    buf[i] = (int16_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  audio_push_samples(buf, count / 2);
  return 0;
}

static int l_audio_ringFree(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)audio_ring_free());
  return 1;
}

static const luaL_Reg l_audio_lib[] = {
    {"playTone", l_audio_playTone},
    {"stopTone", l_audio_stopTone},
    {"setVolume", l_audio_setVolume},
    {"startStream", l_audio_startStream},
    {"stopStream", l_audio_stopStream},
    {"pushSamples", l_audio_pushSamples},
    {"ringFree", l_audio_ringFree},
    {NULL, NULL}};

void lua_bridge_audio_init(lua_State *L) { register_subtable(L, "audio", l_audio_lib); }
