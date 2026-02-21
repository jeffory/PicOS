#include "lua_bridge_internal.h"

// ── picocalc.config.*
// ─────────────────────────────────────────────────────────

static int l_config_get(lua_State *L) {
  const char *key = luaL_checkstring(L, 1);
  const char *val = config_get(key);
  if (val)
    lua_pushstring(L, val);
  else
    lua_pushnil(L);
  return 1;
}

static int l_config_set(lua_State *L) {
  const char *key = luaL_checkstring(L, 1);
  // Allow nil/absent second arg to delete the key
  const char *val = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
  config_set(key, val);
  return 0;
}

static int l_config_save(lua_State *L) {
  lua_pushboolean(L, config_save());
  return 1;
}

static int l_config_load(lua_State *L) {
  lua_pushboolean(L, config_load());
  return 1;
}

static const luaL_Reg l_config_lib[] = {{"get", l_config_get},
                                        {"set", l_config_set},
                                        {"save", l_config_save},
                                        {"load", l_config_load},
                                        {NULL, NULL}};


void lua_bridge_config_init(lua_State *L) {
  register_subtable(L, "config", l_config_lib);
}
