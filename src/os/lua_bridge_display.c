#include "lua_bridge_internal.h"
#include "toast.h"

// ── picocalc.display.* ───────────────────────────────────────────────────────

static int l_display_clear(lua_State *L) {
  uint16_t color = (lua_gettop(L) >= 1) ? l_checkcolor(L, 1) : COLOR_BLACK;
  display_clear(color);
  return 0;
}

static int l_display_setPixel(lua_State *L) {
  int x = (int)luaL_checknumber(L, 1);
  int y = (int)luaL_checknumber(L, 2);
  uint16_t c = l_checkcolor(L, 3);
  display_set_pixel(x, y, c);
  return 0;
}

static int l_display_fillRect(lua_State *L) {
  display_fill_rect((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                    (int)luaL_checknumber(L, 3), (int)luaL_checknumber(L, 4),
                    l_checkcolor(L, 5));
  return 0;
}

static int l_display_drawRect(lua_State *L) {
  display_draw_rect((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                    (int)luaL_checknumber(L, 3), (int)luaL_checknumber(L, 4),
                    l_checkcolor(L, 5));
  return 0;
}

static int l_display_drawLine(lua_State *L) {
  display_draw_line((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                    (int)luaL_checknumber(L, 3), (int)luaL_checknumber(L, 4),
                    l_checkcolor(L, 5));
  return 0;
}

static int l_display_drawCircle(lua_State *L) {
  display_draw_circle((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                      (int)luaL_checknumber(L, 3), l_checkcolor(L, 4));
  return 0;
}

static int l_display_fillCircle(lua_State *L) {
  display_fill_circle((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                      (int)luaL_checknumber(L, 3), l_checkcolor(L, 4));
  return 0;
}

static int l_display_setScrollArea(lua_State *L) {
  int top = (int)luaL_checkinteger(L, 1);
  int height = (int)luaL_checkinteger(L, 2);
  int bottom = (int)luaL_checkinteger(L, 3);
  display_set_scroll_area(top, height, bottom);
  return 0;
}

static int l_display_setScrollOffset(lua_State *L) {
  display_set_scroll_offset((int)luaL_checkinteger(L, 1));
  return 0;
}

static int l_display_drawText(lua_State *L) {
  int x = (int)luaL_checknumber(L, 1);
  int y = (int)luaL_checknumber(L, 2);
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
  toast_draw();
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

static int l_display_setFont(lua_State *L) {
  display_set_font((int)luaL_checkinteger(L, 1));
  return 0;
}

static int l_display_getFont(lua_State *L) {
  lua_pushinteger(L, display_get_font());
  return 1;
}

static int l_display_getFontWidth(lua_State *L) {
  lua_pushinteger(L, display_get_font_width());
  return 1;
}

static int l_display_getFontHeight(lua_State *L) {
  lua_pushinteger(L, display_get_font_height());
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

// picocalc.display.applyEffect(name, ...)
// Dispatches to the appropriate effect function based on the string name.
static int l_display_applyEffect(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);

  if (strcmp(name, "invert") == 0) {
    display_effect_invert();
  } else if (strcmp(name, "darken") == 0) {
    uint8_t factor = (uint8_t)luaL_optinteger(L, 2, 128);
    display_effect_darken(factor);
  } else if (strcmp(name, "brighten") == 0) {
    uint8_t factor = (uint8_t)luaL_optinteger(L, 2, 128);
    display_effect_brighten(factor);
  } else if (strcmp(name, "tint") == 0) {
    uint8_t r = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t g = (uint8_t)luaL_checkinteger(L, 3);
    uint8_t b = (uint8_t)luaL_checkinteger(L, 4);
    uint8_t strength = (uint8_t)luaL_optinteger(L, 5, 128);
    display_effect_tint(r, g, b, strength);
  } else if (strcmp(name, "fade") == 0) {
    uint8_t r = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t g = (uint8_t)luaL_checkinteger(L, 3);
    uint8_t b = (uint8_t)luaL_checkinteger(L, 4);
    uint8_t factor = (uint8_t)luaL_optinteger(L, 5, 128);
    display_effect_tint(r, g, b, factor); // fade = tint toward target color
  } else if (strcmp(name, "grayscale") == 0) {
    display_effect_grayscale();
  } else if (strcmp(name, "blend") == 0) {
    // Arg 2: image userdata, Arg 3: alpha (0-255)
    lua_image_t *img = (lua_image_t *)luaL_checkudata(L, 2, GRAPHICS_IMAGE_MT);
    if (!img->data) {
      return luaL_error(L, "blend: image has been freed");
    }
    uint8_t alpha = (uint8_t)luaL_optinteger(L, 3, 128);
    display_effect_blend(img->data, img->w, img->h, alpha);
  } else if (strcmp(name, "palette") == 0) {
    // Arg 2: table of 256 RGB565 color values
    luaL_checktype(L, 2, LUA_TTABLE);
    int lut_size = (int)luaL_len(L, 2);
    if (lut_size <= 0 || lut_size > 256) {
      return luaL_error(L, "palette: table must have 1-256 entries");
    }
    // Allocate LUT on stack (256 * 2 = 512 bytes, fine for stack)
    uint16_t lut[256];
    for (int i = 0; i < lut_size; i++) {
      lua_rawgeti(L, 2, i + 1);
      lut[i] = (uint16_t)lua_tointeger(L, -1);
      lua_pop(L, 1);
    }
    display_effect_palette(lut, lut_size);
  } else if (strcmp(name, "dither") == 0) {
    uint8_t levels = (uint8_t)luaL_optinteger(L, 2, 4);
    display_effect_dither(levels);
  } else if (strcmp(name, "scanline") == 0) {
    uint8_t intensity = (uint8_t)luaL_optinteger(L, 2, 128);
    display_effect_scanline(intensity);
  } else if (strcmp(name, "posterize") == 0) {
    uint8_t levels = (uint8_t)luaL_optinteger(L, 2, 4);
    display_effect_posterize(levels);
  } else {
    return luaL_error(L, "unknown effect: %s", name);
  }
  return 0;
}

static int l_display_fillVLine(lua_State *L) {
  display_fill_vline((int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
                     (int)luaL_checknumber(L, 3),
                     (uint16_t)luaL_checkinteger(L, 4));
  return 0;
}

static int l_display_drawTexturedColumn(lua_State *L) {
  int x = (int)luaL_checknumber(L, 1);
  int y0 = (int)luaL_checknumber(L, 2);
  int y1 = (int)luaL_checknumber(L, 3);
  lua_image_t *img =
      (lua_image_t *)luaL_checkudata(L, 4, GRAPHICS_IMAGE_MT);
  if (!img->data)
    return luaL_error(L, "invalid image");
  int tex_x = (int)luaL_checknumber(L, 5);
  int tex_y0 = (int)luaL_checknumber(L, 6);
  int tex_y1 = (int)luaL_checknumber(L, 7);
  display_draw_textured_column(x, y0, y1, img->data, img->w, img->h, tex_x,
                               tex_y0, tex_y1);
  return 0;
}

static int l_display_fillVLineGradient(lua_State *L) {
  display_fill_vline_gradient(
      (int)luaL_checknumber(L, 1), (int)luaL_checknumber(L, 2),
      (int)luaL_checknumber(L, 3), (uint16_t)luaL_checkinteger(L, 4),
      (uint16_t)luaL_checkinteger(L, 5));
  return 0;
}

static const luaL_Reg l_display_lib[] = {
    {"clear", l_display_clear},
    {"setPixel", l_display_setPixel},
    {"fillRect", l_display_fillRect},
    {"drawRect", l_display_drawRect},
    {"drawLine", l_display_drawLine},
    {"drawCircle", l_display_drawCircle},
    {"fillCircle", l_display_fillCircle},
    {"setScrollArea", l_display_setScrollArea},
    {"setScrollOffset", l_display_setScrollOffset},
    {"drawText", l_display_drawText},
    {"flush", l_display_flush},
    {"getWidth", l_display_getWidth},
    {"getHeight", l_display_getHeight},
    {"setBrightness", l_display_setBrightness},
    {"textWidth", l_display_textWidth},
    {"setFont", l_display_setFont},
    {"getFont", l_display_getFont},
    {"getFontWidth", l_display_getFontWidth},
    {"getFontHeight", l_display_getFontHeight},
    {"rgb", l_display_rgb},
    {"applyEffect", l_display_applyEffect},
    {"fillVLine", l_display_fillVLine},
    {"drawTexturedColumn", l_display_drawTexturedColumn},
    {"fillVLineGradient", l_display_fillVLineGradient},
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
  // Font ID constants
  lua_pushinteger(L, 0);
  lua_setfield(L, -2, "FONT_6X8");
  lua_pushinteger(L, 1);
  lua_setfield(L, -2, "FONT_8X12");
  lua_pushinteger(L, 2);
  lua_setfield(L, -2, "FONT_SCIENTIFICA");
  lua_pushinteger(L, 3);
  lua_setfield(L, -2, "FONT_SCIENTIFICA_BOLD");
  lua_pop(L, 1);
}
