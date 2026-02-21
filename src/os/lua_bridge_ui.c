#include "lua_bridge_internal.h"

// ── picocalc.ui.*
// ─────────────────────────────────────────────────────────────

static int l_ui_drawHeader(lua_State *L) {
  const char *title = luaL_checkstring(L, 1);
  ui_draw_header(title);
  return 0;
}

static int l_ui_drawFooter(lua_State *L) {
  const char *left = luaL_optstring(L, 1, NULL);
  const char *right = luaL_optstring(L, 2, NULL);
  ui_draw_footer(left, right);
  return 0;
}

static const luaL_Reg l_ui_lib[] = {{"drawHeader", l_ui_drawHeader},
                                    {"drawFooter", l_ui_drawFooter},
                                    {NULL, NULL}};


void lua_bridge_ui_init(lua_State *L) {
  register_subtable(L, "ui", l_ui_lib);
}
