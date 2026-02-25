#include "lua_bridge_internal.h"
#include "../../third_party/lua-5.4/src/lauxlib.h"
#include "umm_malloc.h"

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

static int l_ui_drawTabs(lua_State *L) {
  int y = (int)luaL_checkinteger(L, 1);
  luaL_checktype(L, 2, LUA_TTABLE);  // tabs array
  int active_index = (int)luaL_checkinteger(L, 3);
  int prev_key = (int)luaL_optinteger(L, 4, 0);
  int next_key = (int)luaL_optinteger(L, 5, 0);

  // Convert to 0-based index for C
  active_index--;

  // Count tabs
  int count = (int)lua_rawlen(L, 2);
  if (count == 0) {
    lua_pushinteger(L, active_index + 1); // Return 1-based
    lua_pushinteger(L, 0);
    return 2;
  }

  // Allocate tab strings array
  const char **tabs = (const char **)umm_malloc(sizeof(char *) * count);
  if (!tabs) {
    return luaL_error(L, "Failed to allocate memory for tabs");
  }

  // Extract tab strings from Lua table
  for (int i = 0; i < count; i++) {
    lua_rawgeti(L, 2, i + 1);  // Lua arrays are 1-indexed
    tabs[i] = lua_tostring(L, -1);
    lua_pop(L, 1);
  }

  // Check for key presses if keys specified
  if (prev_key > 0 || next_key > 0) {
    int pressed = kbd_get_buttons_pressed();
    if (prev_key > 0 && (pressed & prev_key)) {
      active_index--;
      if (active_index < 0)
        active_index = count - 1;
    }
    if (next_key > 0 && (pressed & next_key)) {
      active_index++;
      if (active_index >= count)
        active_index = 0;
    }
  }

  // Draw tabs
  int height = ui_draw_tabs(tabs, count, active_index, y);

  umm_free(tabs);

  // Return new active index (1-based) and height consumed
  lua_pushinteger(L, active_index + 1);
  lua_pushinteger(L, height);
  return 2;
}

static const luaL_Reg l_ui_lib[] = {{"drawHeader", l_ui_drawHeader},
                                    {"drawFooter", l_ui_drawFooter},
                                    {"drawTabs", l_ui_drawTabs},
                                    {NULL, NULL}};


void lua_bridge_ui_init(lua_State *L) {
  register_subtable(L, "ui", l_ui_lib);
}
