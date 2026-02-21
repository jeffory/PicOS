#include "lua_bridge_internal.h"

// ── picocalc.display.* ───────────────────────────────────────────────────────

static int l_display_clear(lua_State *L) {
  uint16_t color = (lua_gettop(L) >= 1) ? l_checkcolor(L, 1) : COLOR_BLACK;
  display_clear(color);
  return 0;
}

static int l_display_setPixel(lua_State *L) {
  int x = luaL_checkinteger(L, 1);
  int y = luaL_checkinteger(L, 2);
  uint16_t c = l_checkcolor(L, 3);
  display_set_pixel(x, y, c);
  return 0;
}

static int l_display_fillRect(lua_State *L) {
  display_fill_rect(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
                    luaL_checkinteger(L, 3), luaL_checkinteger(L, 4),
                    l_checkcolor(L, 5));
  return 0;
}

static int l_display_drawRect(lua_State *L) {
  display_draw_rect(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
                    luaL_checkinteger(L, 3), luaL_checkinteger(L, 4),
                    l_checkcolor(L, 5));
  return 0;
}

static int l_display_drawLine(lua_State *L) {
  display_draw_line(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2),
                    luaL_checkinteger(L, 3), luaL_checkinteger(L, 4),
                    l_checkcolor(L, 5));
  return 0;
}

static int l_display_drawText(lua_State *L) {
  int x = luaL_checkinteger(L, 1);
  int y = luaL_checkinteger(L, 2);
  const char *text = luaL_checkstring(L, 3);
  uint16_t fg = l_checkcolor(L, 4);
  uint16_t bg = (lua_gettop(L) >= 5) ? l_checkcolor(L, 5) : COLOR_BLACK;
  int width = display_draw_text(x, y, text, fg, bg);
  lua_pushinteger(L, width);
  return 1;
}

// Set by menu_lua_hook when a screenshot is requested.  Cleared and fired
// inside l_display_flush so the capture always happens on a complete frame.
bool s_screenshot_pending = false;

static int l_display_flush(lua_State *L) {
  (void)L;
  display_flush();
  if (s_screenshot_pending) {
    s_screenshot_pending = false;
    screenshot_save();
  }
  return 0;
}

static int l_display_getWidth(lua_State *L) {
  lua_pushinteger(L, FB_WIDTH);
  return 1;
}

static int l_display_getHeight(lua_State *L) {
  lua_pushinteger(L, FB_HEIGHT);
  return 1;
}

static int l_display_setBrightness(lua_State *L) {
  display_set_brightness((uint8_t)luaL_checkinteger(L, 1));
  return 0;
}

static int l_display_textWidth(lua_State *L) {
  lua_pushinteger(L, display_text_width(luaL_checkstring(L, 1)));
  return 1;
}

// Convenience: create RGB565 from r,g,b components
static int l_display_rgb(lua_State *L) {
  int r = luaL_checkinteger(L, 1);
  int g = luaL_checkinteger(L, 2);
  int b = luaL_checkinteger(L, 3);
  lua_pushinteger(L, RGB565(r, g, b));
  return 1;
}

static const luaL_Reg l_display_lib[] = {
    {"clear", l_display_clear},
    {"setPixel", l_display_setPixel},
    {"fillRect", l_display_fillRect},
    {"drawRect", l_display_drawRect},
    {"drawLine", l_display_drawLine},
    {"drawText", l_display_drawText},
    {"flush", l_display_flush},
    {"getWidth", l_display_getWidth},
    {"getHeight", l_display_getHeight},
    {"setBrightness", l_display_setBrightness},
    {"textWidth", l_display_textWidth},
    {"rgb", l_display_rgb},
    {NULL, NULL}};


void lua_bridge_display_init(lua_State *L) {
  register_subtable(L, "display", l_display_lib);
  // Push colour constants into picocalc.display
  lua_getfield(L, -1, "display");
  lua_pushinteger(L, COLOR_BLACK);
  lua_setfield(L, -2, "BLACK");
  lua_pushinteger(L, COLOR_WHITE);
  lua_setfield(L, -2, "WHITE");
  lua_pushinteger(L, COLOR_RED);
  lua_setfield(L, -2, "RED");
  lua_pushinteger(L, COLOR_GREEN);
  lua_setfield(L, -2, "GREEN");
  lua_pushinteger(L, COLOR_BLUE);
  lua_setfield(L, -2, "BLUE");
  lua_pushinteger(L, COLOR_YELLOW);
  lua_setfield(L, -2, "YELLOW");
  lua_pushinteger(L, COLOR_CYAN);
  lua_setfield(L, -2, "CYAN");
  lua_pushinteger(L, COLOR_GRAY);
  lua_setfield(L, -2, "GRAY");
  lua_pop(L, 1);
}
