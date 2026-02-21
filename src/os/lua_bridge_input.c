#include "lua_bridge_internal.h"

// ── picocalc.input.* ─────────────────────────────────────────────────────────

static int l_input_getButtons(lua_State *L) {
  lua_pushinteger(L, kbd_get_buttons());
  return 1;
}

static int l_input_getButtonsPressed(lua_State *L) {
  lua_pushinteger(L, kbd_get_buttons_pressed());
  return 1;
}

static int l_input_getButtonsReleased(lua_State *L) {
  lua_pushinteger(L, kbd_get_buttons_released());
  return 1;
}

static int l_input_getChar(lua_State *L) {
  char c = kbd_get_char();
  if (c) {
    char s[2] = {c, '\0'};
    lua_pushstring(L, s);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

static int l_input_update(lua_State *L) {
  kbd_poll();
  // Bypass the 256-opcode Lua hook latency by serving the system menu
  // instantly if a button press was detected during this explicit update.
  if (kbd_consume_menu_press()) {
    system_menu_show(L);
  }
  return 0;
}

static int l_input_getRawKey(lua_State *L) {
  lua_pushinteger(L, kbd_get_raw_key());
  return 1;
}

static const luaL_Reg l_input_lib[] = {
    {"update", l_input_update},
    {"getButtons", l_input_getButtons},
    {"getButtonsPressed", l_input_getButtonsPressed},
    {"getButtonsReleased", l_input_getButtonsReleased},
    {"getChar", l_input_getChar},
    {"getRawKey", l_input_getRawKey},
    {NULL, NULL}};


void lua_bridge_input_init(lua_State *L) {
  register_subtable(L, "input", l_input_lib);
  // Push button constants into picocalc.input
  lua_getfield(L, -1, "input");
  lua_pushinteger(L, BTN_UP); lua_setfield(L, -2, "BTN_UP");
  lua_pushinteger(L, BTN_DOWN); lua_setfield(L, -2, "BTN_DOWN");
  lua_pushinteger(L, BTN_LEFT); lua_setfield(L, -2, "BTN_LEFT");
  lua_pushinteger(L, BTN_RIGHT); lua_setfield(L, -2, "BTN_RIGHT");
  lua_pushinteger(L, BTN_ENTER); lua_setfield(L, -2, "BTN_ENTER");
  lua_pushinteger(L, BTN_ESC); lua_setfield(L, -2, "BTN_ESC");
  lua_pushinteger(L, BTN_MENU); lua_setfield(L, -2, "BTN_MENU");
  lua_pushinteger(L, BTN_F1); lua_setfield(L, -2, "BTN_F1");
  lua_pushinteger(L, BTN_F2); lua_setfield(L, -2, "BTN_F2");
  lua_pushinteger(L, BTN_F3); lua_setfield(L, -2, "BTN_F3");
  lua_pushinteger(L, BTN_F4); lua_setfield(L, -2, "BTN_F4");
  lua_pushinteger(L, BTN_F5); lua_setfield(L, -2, "BTN_F5");
  lua_pushinteger(L, BTN_F6); lua_setfield(L, -2, "BTN_F6");
  lua_pushinteger(L, BTN_F7); lua_setfield(L, -2, "BTN_F7");
  lua_pushinteger(L, BTN_F8); lua_setfield(L, -2, "BTN_F8");
  lua_pushinteger(L, BTN_F9); lua_setfield(L, -2, "BTN_F9");
  lua_pushinteger(L, BTN_BACKSPACE); lua_setfield(L, -2, "BTN_BACKSPACE");
  lua_pushinteger(L, BTN_TAB); lua_setfield(L, -2, "BTN_TAB");
  lua_pushinteger(L, BTN_DEL); lua_setfield(L, -2, "BTN_DEL");
  lua_pushinteger(L, BTN_SHIFT); lua_setfield(L, -2, "BTN_SHIFT");
  lua_pushinteger(L, BTN_CTRL); lua_setfield(L, -2, "BTN_CTRL");
  lua_pushinteger(L, BTN_ALT); lua_setfield(L, -2, "BTN_ALT");
  lua_pushinteger(L, BTN_FN); lua_setfield(L, -2, "BTN_FN");
  lua_pop(L, 1);
}
