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

static const luaL_Reg l_audio_lib[] = {
    {"playTone", l_audio_playTone},
    {"stopTone", l_audio_stopTone},
    {"setVolume", l_audio_setVolume},
    {NULL, NULL}};

void lua_bridge_audio_init(lua_State *L) { register_subtable(L, "audio", l_audio_lib); }
