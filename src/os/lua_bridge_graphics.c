#include "lua_bridge_internal.h"
#include "pico/time.h"
#include <math.h>

// ── picocalc.graphics.* ──────────────────────────────────────────────────────

#define GRAPHICS_IMAGE_MT "picocalc.graphics.image"

static uint16_t s_graphics_color = COLOR_WHITE;
static uint16_t s_graphics_bg_color = COLOR_BLACK;

typedef struct {
  int w;
  int h;
  uint16_t *data;
} lua_image_t;

static lua_image_t *check_image(lua_State *L, int idx) {
  return (lua_image_t *)luaL_checkudata(L, idx, GRAPHICS_IMAGE_MT);
}

static int l_graphics_image_gc(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  if (img->data) {
    umm_free(img->data);
    img->data = NULL;
  }
  return 0;
}

static int l_graphics_setColor(lua_State *L) {
  s_graphics_color = l_checkcolor(L, 1);
  return 0;
}

static int l_graphics_setBackgroundColor(lua_State *L) {
  s_graphics_bg_color = l_checkcolor(L, 1);
  return 0;
}

static int l_graphics_setTransparentColor(lua_State *L) {
  uint16_t color = 0;
  if (!lua_isnil(L, 1))
    color = l_checkcolor(L, 1);
  display_set_transparent_color(color);
  return 0;
}

static int l_graphics_getTransparentColor(lua_State *L) {
  uint16_t color = display_get_transparent_color();
  if (color == 0)
    lua_pushnil(L);
  else
    lua_pushinteger(L, color);
  return 1;
}

static int l_graphics_clear(lua_State *L) {
  uint16_t color =
      (lua_gettop(L) >= 1) ? l_checkcolor(L, 1) : s_graphics_bg_color;
  display_clear(color);
  return 0;
}

static int l_graphics_image_new(lua_State *L) {
  int w = luaL_checkinteger(L, 1);
  int h = luaL_checkinteger(L, 2);
  if (w <= 0 || h <= 0)
    return luaL_error(L, "invalid image dimensions");

  lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
  img->w = w;
  img->h = h;
  img->data = (uint16_t *)umm_malloc(w * h * sizeof(uint16_t));
  if (!img->data)
    return luaL_error(L, "out of memory allocating image");

  memset(img->data, 0, w * h * sizeof(uint16_t));

  luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
  return 1;
}

static int l_graphics_image_load(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);

  if (!fs_sandbox_check(L, path, false)) {
    return luaL_error(L, "access denied");
  }

  sdfile_t f = sdcard_fopen(path, "r");
  if (!f)
    return luaL_error(L, "file not found");

  uint8_t header[16];
  if (sdcard_fread(f, header, 16) != 16) {
    sdcard_fclose(f);
    return luaL_error(L, "invalid or empty file");
  }

  // Magic byte checks
  bool is_bmp = (header[0] == 'B' && header[1] == 'M');
  bool is_jpeg = (header[0] == 0xFF && header[1] == 0xD8);
  bool is_png = (header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E &&
                 header[3] == 0x47);
  bool is_gif = (header[0] == 'G' && header[1] == 'I' && header[2] == 'F');

  if (!is_bmp && !is_jpeg && !is_png && !is_gif) {
    sdcard_fclose(f);
    return luaL_error(L, "unsupported image format");
  }

  if (is_bmp) {
    sdcard_fseek(f, 0);
    uint8_t full_header[54];
    if (sdcard_fread(f, full_header, 54) != 54) {
      sdcard_fclose(f);
      return luaL_error(L, "invalid BMP format");
    }

    uint32_t data_offset = *(uint32_t *)&full_header[10];
    int w = *(int32_t *)&full_header[18];
    int h = *(int32_t *)&full_header[22];
    uint16_t bpp = *(uint16_t *)&full_header[28];
    uint32_t compression = *(uint32_t *)&full_header[30];

    if (compression != 0 && compression != 3) {
      sdcard_fclose(f);
      return luaL_error(L, "unsupported BMP compression");
    }

    if (bpp != 16 && bpp != 24 && bpp != 32) {
      sdcard_fclose(f);
      return luaL_error(L, "unsupported BMP depth (%d bpp)", bpp);
    }

    bool flip_y = true;
    if (h < 0) {
      h = -h;
      flip_y = false;
    }

    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
      sdcard_fclose(f);
      return luaL_error(L, "invalid BMP dimensions");
    }

    lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
    img->w = w;
    img->h = h;
    img->data = (uint16_t *)umm_malloc(w * h * sizeof(uint16_t));
    if (!img->data) {
      sdcard_fclose(f);
      return luaL_error(L, "out of memory allocating image");
    }
    luaL_setmetatable(L, GRAPHICS_IMAGE_MT);

    sdcard_fseek(f, data_offset);

    int row_bytes = ((w * bpp + 31) / 32) * 4;
    uint8_t *row_buf = (uint8_t *)umm_malloc(row_bytes);
    if (!row_buf) {
      umm_free(img->data);
      sdcard_fclose(f);
      return luaL_error(L, "out of memory allocating row buffer");
    }

    for (int y = 0; y < h; y++) {
      int dest_y = flip_y ? (h - 1 - y) : y;
      if (sdcard_fread(f, row_buf, row_bytes) != row_bytes)
        break;

      for (int x = 0; x < w; x++) {
        uint16_t color = 0;
        if (bpp == 24) {
          uint8_t b = row_buf[x * 3];
          uint8_t g = row_buf[x * 3 + 1];
          uint8_t r = row_buf[x * 3 + 2];
          color = RGB565(r, g, b);
        } else if (bpp == 32) {
          uint8_t b = row_buf[x * 4];
          uint8_t g = row_buf[x * 4 + 1];
          uint8_t r = row_buf[x * 4 + 2];
          color = RGB565(r, g, b);
        } else if (bpp == 16) {
          uint16_t p = *(uint16_t *)&row_buf[x * 2];
          color = p;
        }
        img->data[dest_y * w + x] = color;
      }
    }

    umm_free(row_buf);
    sdcard_fclose(f);
    return 1;
  }

  // BMP wasn't matched. We must close our original handle so the decoders can
  // open their own.
  sdcard_fclose(f);

  image_decode_result_t res = {0, 0, NULL};
  bool success = false;
  const char *err_msg = "unsupported image format";

  if (is_jpeg) {
    success = decode_jpeg_file(path, &res);
    err_msg = "JPEG decoding failed";
  } else if (is_png) {
    success = decode_png_file(path, &res);
    err_msg = "PNG decoding failed";
  } else if (is_gif) {
    success = decode_gif_file(path, &res);
    err_msg = "GIF decoding failed";
  }

  // Return userdata holding the memory if decoder succeeded
  if (success && res.data) {
    lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
    img->w = res.w;
    img->h = res.h;
    img->data = res.data; // Now managed by lua_image_t gc handler
    luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
    return 1;
  }

  // If decoding failed res.data (from umm_malloc) needs to be freed.
  if (res.data) {
    umm_free(res.data);
  }
  return luaL_error(L, "%s", err_msg);
}

static int l_graphics_image_getSize(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  lua_pushinteger(L, img->w);
  lua_pushinteger(L, img->h);
  return 2;
}

static int l_graphics_image_copy(lua_State *L) {
  lua_image_t *src = check_image(L, 1);
  lua_image_t *dst = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
  dst->w = src->w;
  dst->h = src->h;
  dst->data = (uint16_t *)umm_malloc(dst->w * dst->h * sizeof(uint16_t));
  if (!dst->data)
    return luaL_error(L, "out of memory allocating image copy");
  memcpy(dst->data, src->data, dst->w * dst->h * sizeof(uint16_t));
  luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
  return 1;
}

static int l_graphics_image_draw(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);

  bool flip_x = false;
  bool flip_y = false;
  if (lua_istable(L, 4)) {
    lua_getfield(L, 4, "flipX");
    flip_x = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "flipY");
    flip_y = lua_toboolean(L, -1);
    lua_pop(L, 1);
  } else if (lua_isboolean(L, 4)) {
    flip_x = lua_toboolean(L, 4);
  }

  int sx = 0, sy = 0, sw = img->w, sh = img->h;
  if (lua_istable(L, 5)) {
    lua_getfield(L, 5, "x");
    sx = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_getfield(L, 5, "y");
    sy = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_getfield(L, 5, "w");
    sw = luaL_optinteger(L, -1, img->w);
    lua_pop(L, 1);
    lua_getfield(L, 5, "h");
    sh = luaL_optinteger(L, -1, img->h);
    lua_pop(L, 1);
  }

  display_draw_image_partial(x, y, img->w, img->h, img->data, sx, sy, sw, sh,
                             flip_x, flip_y, 0);
  return 0;
}

static int l_graphics_image_drawAnchored(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);
  double ax = luaL_checknumber(L, 4);
  double ay = luaL_checknumber(L, 5);

  x -= (int)(img->w * ax);
  y -= (int)(img->h * ay);

  display_draw_image_partial(x, y, img->w, img->h, img->data, 0, 0, img->w,
                             img->h, false, false, 0);
  return 0;
}

static int l_graphics_image_drawTiled(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);
  int rect_w = luaL_checkinteger(L, 4);
  int rect_h = luaL_checkinteger(L, 5);

  for (int ty = 0; ty < rect_h; ty += img->h) {
    for (int tx = 0; tx < rect_w; tx += img->w) {
      int draw_w = (tx + img->w > rect_w) ? (rect_w - tx) : img->w;
      int draw_h = (ty + img->h > rect_h) ? (rect_h - ty) : img->h;
      display_draw_image_partial(x + tx, y + ty, img->w, img->h, img->data, 0,
                                 0, draw_w, draw_h, false, false, 0);
    }
  }

  return 0;
}

static int l_graphics_image_setStorageLocation(lua_State *L) {
  return luaL_error(L, "setStorageLocation not implemented yet");
}

static int l_graphics_image_getMetadata(lua_State *L) {
  return luaL_error(L, "getMetadata not implemented yet");
}

static int l_graphics_image_drawScaled(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);
  float scale = luaL_checknumber(L, 4);
  float angle = luaL_optnumber(L, 5, 0.0);

  display_draw_image_scaled(x, y, img->w, img->h, img->data, scale, angle, 0);
  return 0;
}

static int l_graphics_image_drawScaledNN(lua_State *L) {
  lua_image_t *img = check_image(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);
  int scale = luaL_checkinteger(L, 4);

  if (scale <= 0)
    return luaL_error(L, "scale must be positive integer");

  int dst_w = img->w * scale;
  int dst_h = img->h * scale;

  display_draw_image_scaled_nn(x, y, img->data, img->w, img->h, dst_w, dst_h, 0);
  return 0;
}

static const luaL_Reg l_graphics_image_methods[] = {
    {"getSize", l_graphics_image_getSize},
    {"copy", l_graphics_image_copy},
    {"draw", l_graphics_image_draw},
    {"drawAnchored", l_graphics_image_drawAnchored},
    {"drawTiled", l_graphics_image_drawTiled},
    {"drawScaled", l_graphics_image_drawScaled},
    {"drawScaledNN", l_graphics_image_drawScaledNN},
    {"setStorageLocation", l_graphics_image_setStorageLocation},
    {"getMetadata", l_graphics_image_getMetadata},
    {NULL, NULL}};

static int l_graphics_image_loadFromBuffer(lua_State *L) {
  size_t len;
  const uint8_t *data;

  if (lua_isstring(L, 1)) {
    data = (const uint8_t *)luaL_checklstring(L, 1, &len);
  } else if (lua_isuserdata(L, 1)) {
    data = (const uint8_t *)lua_touserdata(L, 1);
    len = (size_t)luaL_checkinteger(L, 2);
  } else {
    return luaL_error(L, "expected string or userdata containing file buffer");
  }

  if (!data || len < 16) {
    return luaL_error(L, "buffer too small or invalid");
  }

  bool is_bmp = (data[0] == 'B' && data[1] == 'M');
  bool is_jpeg = (data[0] == 0xFF && data[1] == 0xD8);
  bool is_png = (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E &&
                 data[3] == 0x47);
  bool is_gif = (data[0] == 'G' && data[1] == 'I' && data[2] == 'F');

  if (is_bmp) {
    return luaL_error(L, "BMP from buffer not supported yet");
  }

  image_decode_result_t res = {0, 0, NULL};
  bool success = false;
  const char *err_msg = "unsupported image format";

  if (is_jpeg) {
    success = decode_jpeg_buffer(data, len, &res);
    err_msg = "JPEG decoding failed";
  } else if (is_png) {
    success = decode_png_buffer(data, len, &res);
    err_msg = "PNG decoding failed";
  } else if (is_gif) {
    success = decode_gif_buffer(data, len, &res);
    err_msg = "GIF decoding failed";
  }

  if (success && res.data) {
    lua_image_t *img = (lua_image_t *)lua_newuserdata(L, sizeof(lua_image_t));
    img->w = res.w;
    img->h = res.h;
    img->data = res.data;
    luaL_setmetatable(L, GRAPHICS_IMAGE_MT);
    return 1;
  }

  return luaL_error(L, err_msg);
}

static int l_graphics_image_loadRemote(lua_State *L) {
  return luaL_error(L, "loadRemote not implemented yet");
}

static int l_graphics_image_getInfo(lua_State *L) {
  return luaL_error(L, "getInfo not implemented yet");
}

static int l_graphics_image_loadRegion(lua_State *L) {
  return luaL_error(L, "loadRegion not implemented yet");
}

static int l_graphics_image_loadScaled(lua_State *L) {
  return luaL_error(L, "loadScaled not implemented yet");
}

static int l_graphics_image_newStream(lua_State *L) {
  return luaL_error(L, "newStream not implemented yet");
}

static int l_graphics_image_setPlaceholder(lua_State *L) {
  return luaL_error(L, "setPlaceholder not implemented yet");
}

static int l_graphics_image_getSupportedFormats(lua_State *L) {
  lua_newtable(L);
  lua_pushstring(L, "BMP");
  lua_rawseti(L, -2, 1);
  lua_pushstring(L, "JPEG");
  lua_rawseti(L, -2, 2);
  lua_pushstring(L, "PNG");
  lua_rawseti(L, -2, 3);
  lua_pushstring(L, "GIF");
  lua_rawseti(L, -2, 4);
  return 1;
}

static const luaL_Reg l_graphics_image_lib[] = {
    {"new", l_graphics_image_new},
    {"load", l_graphics_image_load},
    {"loadFromBuffer", l_graphics_image_loadFromBuffer},
    {"loadRemote", l_graphics_image_loadRemote},
    {"getInfo", l_graphics_image_getInfo},
    {"loadRegion", l_graphics_image_loadRegion},
    {"loadScaled", l_graphics_image_loadScaled},
    {"newStream", l_graphics_image_newStream},
    {"setPlaceholder", l_graphics_image_setPlaceholder},
    {"getSupportedFormats", l_graphics_image_getSupportedFormats},
    {NULL, NULL}};

#define GRAPHICS_IMAGESTREAM_MT "picocalc.graphics.imagestream"

typedef struct {
  void *stream_ptr; // Stub data
} lua_image_stream_t;

static int l_graphics_imagestream_gc(lua_State *L) {
  (void)L;
  return 0;
}

static int l_graphics_imagestream_getNextTile(lua_State *L) {
  return luaL_error(L, "getNextTile not implemented yet");
}

static int l_graphics_imagestream_isComplete(lua_State *L) {
  lua_pushboolean(L, false); // stub
  return 1;
}

static const luaL_Reg l_graphics_imagestream_methods[] = {
    {"getNextTile", l_graphics_imagestream_getNextTile},
    {"isComplete", l_graphics_imagestream_isComplete},
    {NULL, NULL}};

static int l_graphics_cache_setMaxMemory(lua_State *L) {
  return luaL_error(L, "setMaxMemory not implemented yet");
}

static int l_graphics_cache_retain(lua_State *L) {
  return luaL_error(L, "retain not implemented yet");
}

static int l_graphics_cache_release(lua_State *L) {
  return luaL_error(L, "release not implemented yet");
}

static const luaL_Reg l_graphics_cache_lib[] = {
    {"setMaxMemory", l_graphics_cache_setMaxMemory},
    {"retain", l_graphics_cache_retain},
    {"release", l_graphics_cache_release},
    {NULL, NULL}};

// drawGrid(x, y, cell_w, cell_h, cols, rows, color)
// Draws a grid of cols×rows cells in a single C call.
// Replaces cols*rows individual drawRect calls from Lua.
static int l_graphics_drawGrid(lua_State *L) {
  int x      = luaL_checkinteger(L, 1);
  int y      = luaL_checkinteger(L, 2);
  int cell_w = luaL_checkinteger(L, 3);
  int cell_h = luaL_checkinteger(L, 4);
  int cols   = luaL_checkinteger(L, 5);
  int rows   = luaL_checkinteger(L, 6);
  uint16_t color = l_checkcolor(L, 7);
  int total_w = cols * cell_w;
  int total_h = rows * cell_h;
  for (int r = 0; r <= rows; r++)
    display_fill_rect(x, y + r * cell_h, total_w, 1, color);
  for (int c = 0; c <= cols; c++)
    display_fill_rect(x + c * cell_w, y, 1, total_h, color);
  return 0;
}

// fillBorderedRect(x, y, w, h, fill_color, border_color)
// Fills a rectangle then draws a 1-pixel border over it in one C call.
// Replaces a fillRect + drawRect pair from Lua.
static int l_graphics_fillBorderedRect(lua_State *L) {
  int x = luaL_checkinteger(L, 1);
  int y = luaL_checkinteger(L, 2);
  int w = luaL_checkinteger(L, 3);
  int h = luaL_checkinteger(L, 4);
  uint16_t fill   = l_checkcolor(L, 5);
  uint16_t border = l_checkcolor(L, 6);
  display_fill_rect(x, y, w, h, fill);
  display_draw_rect(x, y, w, h, border);
  return 0;
}

// updateDrawParticles(flat_array, delta_s) -> live_count
//
// flat_array is a Lua sequence with 6 values per particle:
//   { x, y, vx, vy, life_ms, color,  x, y, vx, vy, life_ms, color, ... }
//
// In a single C call this function:
//   1. Updates each particle's position (x += vx*dt, y += vy*dt)
//   2. Decrements its lifetime (life -= dt*1000)
//   3. Draws a pixel for every particle that is still alive
//   4. Compacts live particles to the front of the array (removes dead ones)
//
// Replaces per-frame Lua loops that otherwise cost one Lua->C call per pixel.
static int l_graphics_updateDrawParticles(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  float dt       = (float)luaL_checknumber(L, 2);
  float life_dec = dt * 1000.0f;

  int n     = (int)lua_rawlen(L, 1); // total values in flat array
  int write = 1;                     // compaction write cursor (1-indexed)

  for (int base = 1; base <= n; base += 6) {
    lua_rawgeti(L, 1, base);     float x    = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 1); float y    = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 2); float vx   = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 3); float vy   = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 4); float life = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, base + 5); uint16_t color = (uint16_t)lua_tointeger(L, -1); lua_pop(L, 1);

    x    += vx * dt;
    y    += vy * dt;
    life -= life_dec;

    if (life > 0.0f) {
      display_set_pixel((int)x, (int)y, color);
      lua_pushnumber(L,  x);     lua_rawseti(L, 1, write++);
      lua_pushnumber(L,  y);     lua_rawseti(L, 1, write++);
      lua_pushnumber(L,  vx);    lua_rawseti(L, 1, write++);
      lua_pushnumber(L,  vy);    lua_rawseti(L, 1, write++);
      lua_pushnumber(L,  life);  lua_rawseti(L, 1, write++);
      lua_pushinteger(L, color); lua_rawseti(L, 1, write++);
    }
  }

  // Clear dead-particle slots at the tail so lua_rawlen stays correct.
  for (int i = write; i <= n; i++) {
    lua_pushnil(L);
    lua_rawseti(L, 1, i);
  }

  lua_pushinteger(L, (write - 1) / 6); // return live particle count
  return 1;
}

// draw3DWireframe(verts_flat, edges_flat, angleX, angleY, angleZ,
//                 scx, scy, fov, edge_color [, vert_color, vert_size])
//
// verts_flat  : flat Lua sequence {x1,y1,z1, x2,y2,z2, ...}  (n/3 vertices)
// edges_flat  : flat Lua sequence {a1,b1, a2,b2, ...}         (1-based indices)
// angleX/Y/Z  : rotation angles in radians, applied in X→Y→Z order
// scx, scy    : screen centre pixels
// fov         : perspective distance (e.g. 200)
// edge_color  : RGB565 line colour
// vert_color  : (optional) RGB565 dot colour; dots omitted when absent/0
// vert_size   : (optional) dot size in pixels (default 3)
//
// Computes the combined rotation matrix M = Rz*Ry*Rx once per call (6 trig
// ops), then transforms every vertex with 9 muls + 6 adds, projects to screen,
// and draws all edges and vertex dots — all in a single Lua→C call.
static int l_graphics_draw3DWireframe(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TTABLE);
  float aX  = (float)luaL_checknumber(L, 3);
  float aY  = (float)luaL_checknumber(L, 4);
  float aZ  = (float)luaL_checknumber(L, 5);
  int   scx = luaL_checkinteger(L, 6);
  int   scy = luaL_checkinteger(L, 7);
  float fov = (float)luaL_checknumber(L, 8);
  uint16_t edge_color = l_checkcolor(L, 9);
  uint16_t vert_color = (lua_gettop(L) >= 10) ? l_checkcolor(L, 10) : 0;
  int vert_size       = (lua_gettop(L) >= 11) ? (int)luaL_checkinteger(L, 11) : 3;

  // Build combined rotation matrix M = Rz(aZ) * Ry(aY) * Rx(aX).
  // Applying M*v is equivalent to rotateX then rotateY then rotateZ.
  float cX = cosf(aX), sX = sinf(aX);
  float cY = cosf(aY), sY = sinf(aY);
  float cZ = cosf(aZ), sZ = sinf(aZ);

  float m00 = cZ*cY,               m01 = cZ*sY*sX - sZ*cX,  m02 = cZ*sY*cX + sZ*sX;
  float m10 = sZ*cY,               m11 = sZ*sY*sX + cZ*cX,  m12 = sZ*sY*cX - cZ*sX;
  float m20 = -sY,                 m21 = cY*sX,              m22 = cY*cX;

  // Transform + project all vertices into screen-space integer coords.
  int n_verts = (int)lua_rawlen(L, 1) / 3;
  if (n_verts > 64) n_verts = 64;
  int px[64], py[64];

  for (int i = 0; i < n_verts; i++) {
    lua_rawgeti(L, 1, i*3 + 1); float vx = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, i*3 + 2); float vy = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_rawgeti(L, 1, i*3 + 3); float vz = (float)lua_tonumber(L, -1); lua_pop(L, 1);

    float rx = m00*vx + m01*vy + m02*vz;
    float ry = m10*vx + m11*vy + m12*vz;
    float rz = m20*vx + m21*vy + m22*vz;

    float scale = fov / (fov + rz);
    px[i] = (int)(scx + rx * scale);
    py[i] = (int)(scy + ry * scale);
  }

  // Draw edges.
  int n_edges_flat = (int)lua_rawlen(L, 2);
  for (int i = 1; i <= n_edges_flat; i += 2) {
    lua_rawgeti(L, 2, i);     int a = (int)lua_tointeger(L, -1) - 1; lua_pop(L, 1);
    lua_rawgeti(L, 2, i + 1); int b = (int)lua_tointeger(L, -1) - 1; lua_pop(L, 1);
    if (a >= 0 && a < n_verts && b >= 0 && b < n_verts)
      display_draw_line(px[a], py[a], px[b], py[b], edge_color);
  }

  // Draw vertex dots (optional).
  if (vert_color && vert_size > 0) {
    int half = vert_size / 2;
    for (int i = 0; i < n_verts; i++)
      display_fill_rect(px[i] - half, py[i] - half, vert_size, vert_size, vert_color);
  }

  return 0;
}

// ── Sprite System ───────────────────────────────────────────────────────────────

#define GRAPHICS_SPRITE_MT "picocalc.graphics.sprite"
#define MAX_SPRITES 256

typedef struct {
  int x, y;
  int width, height;
  int center_x, center_y;
  float scale, scale_y;
  float rotation;
  int z_index;
  bool visible;
  bool updates_enabled;
  bool always_redraw;
  bool redraws_on_image_change;
  bool dirty;
  int tag;
  bool ignores_draw_offset;
  bool opaque;
  bool flip_x, flip_y;
  int bounds_x, bounds_y, bounds_w, bounds_h;
  bool collisions_enabled;
  int collide_x, collide_y, collide_w, collide_h;
  int clip_x, clip_y, clip_w, clip_h;
  bool has_clip;
  int group_mask;
  int collides_with_mask;
  lua_image_t *image;
  uint16_t *frame_data;  // extracted frame pixels (NULL = use full image)
  int frame_w, frame_h;  // dimensions of extracted frame
  int scale_nn;         // integer scale for NN scaling
  bool use_nn_scaling;  // use NN instead of bilinear
  uint16_t transparent_color;  // 0 = use global setting
} lua_sprite_t;

static lua_sprite_t *s_sprites[MAX_SPRITES];
static int s_sprite_count = 0;

static lua_sprite_t *check_sprite(lua_State *L, int idx) {
  return (lua_sprite_t *)luaL_checkudata(L, idx, GRAPHICS_SPRITE_MT);
}

static int l_sprite_new(lua_State *L);
static int l_sprite_add(lua_State *L);
static int l_sprite_remove(lua_State *L);
static int l_sprite_update(lua_State *L);
static int l_sprite_setImage(lua_State *L);
static int l_sprite_getImage(lua_State *L);
static int l_sprite_moveTo(lua_State *L);
static int l_sprite_moveBy(lua_State *L);
static int l_sprite_getPosition(lua_State *L);
static int l_sprite_setZIndex(lua_State *L);
static int l_sprite_getZIndex(lua_State *L);
static int l_sprite_setVisible(lua_State *L);
static int l_sprite_isVisible(lua_State *L);
static int l_sprite_setCenter(lua_State *L);
static int l_sprite_getCenter(lua_State *L);
static int l_sprite_getCenterPoint(lua_State *L);
static int l_sprite_setSize(lua_State *L);
static int l_sprite_getSize(lua_State *L);
static int l_sprite_setScale(lua_State *L);
static int l_sprite_getScale(lua_State *L);
static int l_sprite_setRotation(lua_State *L);
static int l_sprite_setScaleNN(lua_State *L);
static int l_sprite_setTransparentColor(lua_State *L);
static int l_sprite_getRotation(lua_State *L);
static int l_sprite_copy(lua_State *L);
static int l_sprite_setUpdatesEnabled(lua_State *L);
static int l_sprite_updatesEnabled(lua_State *L);
static int l_sprite_setTag(lua_State *L);
static int l_sprite_getTag(lua_State *L);
static int l_sprite_setImageDrawMode(lua_State *L);
static int l_sprite_setImageFlip(lua_State *L);
static int l_sprite_getImageFlip(lua_State *L);
static int l_sprite_setIgnoresDrawOffset(lua_State *L);
static int l_sprite_setBounds(lua_State *L);
static int l_sprite_getBounds(lua_State *L);
static int l_sprite_getBoundsRect(lua_State *L);
static int l_sprite_setOpaque(lua_State *L);
static int l_sprite_isOpaque(lua_State *L);
static int l_sprite_draw(lua_State *L);
static int l_sprite_updateSingle(lua_State *L);
static int l_sprite_setBackgroundDrawingCallback(lua_State *L);
static int l_sprite_index(lua_State *L);
static int l_sprite_newindex(lua_State *L);
static int l_sprite_addSprite(lua_State *L);
static int l_sprite_removeSprite(lua_State *L);
static int l_sprite_getAllSprites(lua_State *L);
static int l_sprite_spriteCount(lua_State *L);
static int l_sprite_removeAll(lua_State *L);
static int l_sprite_removeSprites(lua_State *L);
static int l_sprite_performOnAllSprites(lua_State *L);
static int l_sprite_setCollisionsEnabled(lua_State *L);
static int l_sprite_collisionsEnabled(lua_State *L);
static int l_sprite_setCollideRect(lua_State *L);
static int l_sprite_getCollideRect(lua_State *L);
static int l_sprite_getCollideBounds(lua_State *L);
static int l_sprite_clearCollideRect(lua_State *L);
static int l_sprite_overlappingSprites(lua_State *L);
static int l_sprite_allOverlappingSprites(lua_State *L);
static int l_sprite_setGroups(lua_State *L);
static int l_sprite_setCollidesWithGroups(lua_State *L);
static int l_sprite_setGroupMask(lua_State *L);
static int l_sprite_getGroupMask(lua_State *L);
static int l_sprite_setCollidesWithGroupsMask(lua_State *L);
static int l_sprite_getCollidesWithGroupsMask(lua_State *L);
static int l_sprite_resetGroupMask(lua_State *L);
static int l_sprite_resetCollidesWithGroupsMask(lua_State *L);
static int l_sprite_checkCollisions(lua_State *L);
static int l_sprite_querySpritesAtPoint(lua_State *L);
static int l_sprite_querySpritesInRect(lua_State *L);
static int l_sprite_setClipRect(lua_State *L);
static int l_sprite_clearClipRect(lua_State *L);
static int l_sprite_setAlwaysRedraw(lua_State *L);
static int l_sprite_getAlwaysRedraw(lua_State *L);
static int l_sprite_markDirty(lua_State *L);
static int l_sprite_addDirtyRect(lua_State *L);
static int l_sprite_setRedrawsOnImageChange(lua_State *L);
static int l_sprite_querySpritesAlongLine(lua_State *L);
static int l_sprite_querySpriteInfoAlongLine(lua_State *L);

static int l_sprite_gc(lua_State *L) {
  // getAllSprites() previously pushed proxy full-userdata of sizeof(lua_sprite_t*)
  // bytes with the sprite metatable.  Accessing frame_data (offset ~104) on such
  // a proxy reads past the end of the 4-byte allocation and corrupts the heap.
  // Guard against any undersized userdata as a safety net.
  if ((size_t)lua_rawlen(L, 1) < sizeof(lua_sprite_t)) return 0;
  lua_sprite_t *s = check_sprite(L, 1);
  if (s->frame_data) {
    umm_free(s->frame_data);
    s->frame_data = NULL;
  }
  s->image = NULL;
  return 0;
}

static int l_sprite_new(lua_State *L) {
  lua_sprite_t *s = (lua_sprite_t *)lua_newuserdata(L, sizeof(lua_sprite_t));
  s->x = 0;
  s->y = 0;
  s->width = 0;
  s->height = 0;
  s->center_x = 0;
  s->center_y = 0;
  s->scale = 1.0f;
  s->scale_y = 1.0f;
  s->rotation = 0.0f;
  s->z_index = 0;
  s->visible = true;
  s->updates_enabled = true;
  s->always_redraw = false;
  s->redraws_on_image_change = true;
  s->dirty = false;
  s->tag = 0;
  s->ignores_draw_offset = false;
  s->opaque = true;
  s->flip_x = false;
  s->flip_y = false;
  s->bounds_x = 0;
  s->bounds_y = 0;
  s->bounds_w = 0;
  s->bounds_h = 0;
  s->collisions_enabled = false;
  s->collide_x = 0;
  s->collide_y = 0;
  s->collide_w = 0;
  s->collide_h = 0;
  s->clip_x = 0;
  s->clip_y = 0;
  s->clip_w = 0;
  s->clip_h = 0;
  s->has_clip = false;
  s->group_mask = 0;
  s->collides_with_mask = 0;
  s->image = NULL;
  s->frame_data = NULL;
  s->frame_w = 0;
  s->frame_h = 0;
  s->scale_nn = 1;
  s->use_nn_scaling = false;
  s->transparent_color = 0;  // use global setting

  if (lua_isuserdata(L, 1)) {
    s->image = (lua_image_t *)lua_touserdata(L, 1);
    if (s->image) {
      s->width = s->image->w;
      s->height = s->image->h;
    }
  }

  luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
  return 1;
}

static int l_sprite_add(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (s_sprite_count >= MAX_SPRITES)
    return luaL_error(L, "max sprites reached");
  s_sprites[s_sprite_count++] = s;
  return 0;
}

static int l_sprite_remove(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  for (int i = 0; i < s_sprite_count; i++) {
    if (s_sprites[i] == s) {
      for (int j = i; j < s_sprite_count - 1; j++)
        s_sprites[j] = s_sprites[j + 1];
      s_sprite_count--;
      break;
    }
  }
  return 0;
}

static int l_sprite_update(lua_State *L) {
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    if (s->updates_enabled && s->visible && s->image) {
      int draw_x = s->x;
      int draw_y = s->y;
      // Use extracted frame if available, otherwise the full image
      const uint16_t *data = s->frame_data ? s->frame_data : s->image->data;
      int src_w = s->frame_data ? s->frame_w : s->width;
      int src_h = s->frame_data ? s->frame_h : s->height;
      
      // Handle NN scaling
      if (s->use_nn_scaling && s->scale_nn > 1) {
        int dst_w = src_w * s->scale_nn;
        int dst_h = src_h * s->scale_nn;
        display_draw_image_scaled_nn(draw_x, draw_y, data, src_w, src_h, dst_w, dst_h, s->transparent_color);
      } else if (s->rotation != 0.0f || s->scale != 1.0f || s->scale_y != 1.0f) {
        display_draw_image_scaled(draw_x, draw_y, src_w, src_h,
                                  data, s->scale, s->rotation,
                                  s->transparent_color);
      } else {
        display_draw_image_partial(draw_x, draw_y, src_w, src_h,
                                   data, 0, 0, src_w, src_h,
                                   s->flip_x, s->flip_y, s->transparent_color);
      }
    }
  }
  return 0;
}

static int l_sprite_setImage(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  // Changing the image invalidates any extracted frame
  if (s->frame_data) {
    umm_free(s->frame_data);
    s->frame_data = NULL;
    s->frame_w = 0;
    s->frame_h = 0;
  }
  if (lua_isnil(L, 2)) {
    s->image = NULL;
    s->width = 0;
    s->height = 0;
    return 0;
  }
  s->image = (lua_image_t *)luaL_checkudata(L, 2, GRAPHICS_IMAGE_MT);
  s->width = s->image->w;
  s->height = s->image->h;
  if (lua_isboolean(L, 3))
    s->flip_x = lua_toboolean(L, 3);
  if (lua_isnumber(L, 4))
    s->scale = (float)lua_tonumber(L, 4);
  if (lua_isnumber(L, 5))
    s->scale_y = (float)lua_tonumber(L, 5);
  return 0;
}

static int l_sprite_getImage(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (!s->image)
    return 0;
  lua_pushlightuserdata(L, s->image);
  return 1;
}

static int l_sprite_moveTo(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->x = luaL_checkinteger(L, 2);
  s->y = luaL_checkinteger(L, 3);
  return 0;
}

static int l_sprite_moveBy(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->x += luaL_checkinteger(L, 2);
  s->y += luaL_checkinteger(L, 3);
  return 0;
}

static int l_sprite_getPosition(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->x);
  lua_pushinteger(L, s->y);
  return 2;
}

static int l_sprite_setZIndex(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->z_index = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_getZIndex(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->z_index);
  return 1;
}

static int l_sprite_setVisible(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->visible = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_isVisible(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->visible);
  return 1;
}

static int l_sprite_setCenter(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->center_x = luaL_checkinteger(L, 2);
  s->center_y = luaL_checkinteger(L, 3);
  return 0;
}

static int l_sprite_getCenter(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->center_x);
  lua_pushinteger(L, s->center_y);
  return 2;
}

static int l_sprite_getCenterPoint(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_createtable(L, 2, 0);
  lua_pushinteger(L, s->center_x);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, s->center_y);
  lua_rawseti(L, -2, 2);
  return 1;
}

static int l_sprite_setSize(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->width = luaL_checkinteger(L, 2);
  s->height = luaL_checkinteger(L, 3);
  return 0;
}

static int l_sprite_getSize(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->width);
  lua_pushinteger(L, s->height);
  return 2;
}

static int l_sprite_setScale(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->scale = (float)luaL_checknumber(L, 2);
  s->scale_y = lua_isnumber(L, 3) ? (float)lua_tonumber(L, 3) : s->scale;
  return 0;
}

static int l_sprite_getScale(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushnumber(L, s->scale);
  lua_pushnumber(L, s->scale_y);
  return 2;
}

static int l_sprite_setRotation(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->rotation = (float)luaL_checknumber(L, 2);
  if (lua_isnumber(L, 3))
    s->scale = (float)lua_tonumber(L, 3);
  if (lua_isnumber(L, 4))
    s->scale_y = (float)lua_tonumber(L, 4);
  return 0;
}

static int l_sprite_getRotation(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushnumber(L, s->rotation);
  return 1;
}

static int l_sprite_setScaleNN(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int scale = luaL_checkinteger(L, 2);
  if (scale <= 0)
    return luaL_error(L, "scale must be positive integer");
  
  s->scale_nn = scale;
  s->use_nn_scaling = true;
  return 0;
}

static int l_sprite_setTransparentColor(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (lua_isnil(L, 2)) {
    s->transparent_color = 0;  // use global setting
  } else {
    s->transparent_color = l_checkcolor(L, 2);
  }
  return 0;
}

static int l_sprite_copy(lua_State *L) {
  lua_sprite_t *src = check_sprite(L, 1);
  lua_sprite_t *dst = (lua_sprite_t *)lua_newuserdata(L, sizeof(lua_sprite_t));
  memcpy(dst, src, sizeof(lua_sprite_t));
  // Deep-copy extracted frame data so each sprite owns its buffer
  if (src->frame_data && src->frame_w > 0 && src->frame_h > 0) {
    int sz = src->frame_w * src->frame_h * sizeof(uint16_t);
    dst->frame_data = (uint16_t *)umm_malloc(sz);
    if (dst->frame_data)
      memcpy(dst->frame_data, src->frame_data, sz);
    else
      dst->frame_data = NULL;
  }
  luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
  return 1;
}

static int l_sprite_setSourceRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int sx = luaL_checkinteger(L, 2);
  int sy = luaL_checkinteger(L, 3);
  int sw = luaL_checkinteger(L, 4);
  int sh = luaL_checkinteger(L, 5);

  if (!s->image) return 0;

  // Clamp to image bounds
  if (sx < 0) sx = 0;
  if (sy < 0) sy = 0;
  if (sx + sw > s->image->w) sw = s->image->w - sx;
  if (sy + sh > s->image->h) sh = s->image->h - sy;
  if (sw <= 0 || sh <= 0) return 0;

  if (s->frame_data) {
    umm_free(s->frame_data);
    s->frame_data = NULL;
  }

  s->frame_data = (uint16_t *)umm_malloc(sw * sh * sizeof(uint16_t));
  if (!s->frame_data) return 0;

  for (int row = 0; row < sh; row++)
    for (int col = 0; col < sw; col++)
      s->frame_data[row * sw + col] =
          s->image->data[(sy + row) * s->image->w + (sx + col)];

  s->frame_w = sw;
  s->frame_h = sh;
  return 0;
}

static int l_sprite_clearSourceRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (s->frame_data) {
    umm_free(s->frame_data);
    s->frame_data = NULL;
    s->frame_w = 0;
    s->frame_h = 0;
  }
  return 0;
}

static int l_sprite_setUpdatesEnabled(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->updates_enabled = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_updatesEnabled(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->updates_enabled);
  return 1;
}

static int l_sprite_setTag(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->tag = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_getTag(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->tag);
  return 1;
}

static int l_sprite_setImageDrawMode(lua_State *L) {
  return 0;
}

static int l_sprite_setImageFlip(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->flip_x = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_getImageFlip(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->flip_x);
  return 1;
}

static int l_sprite_setIgnoresDrawOffset(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->ignores_draw_offset = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_setBounds(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "x");
    lua_getfield(L, 2, "y");
    lua_getfield(L, 2, "w");
    lua_getfield(L, 2, "h");
    s->bounds_x = luaL_optinteger(L, -4, 0);
    s->bounds_y = luaL_optinteger(L, -3, 0);
    s->bounds_w = luaL_optinteger(L, -2, 0);
    s->bounds_h = luaL_optinteger(L, -1, 0);
    lua_pop(L, 4);
  } else {
    s->bounds_x = luaL_checkinteger(L, 2);
    s->bounds_y = luaL_checkinteger(L, 3);
    s->bounds_w = luaL_checkinteger(L, 4);
    s->bounds_h = luaL_checkinteger(L, 5);
  }
  return 0;
}

static int l_sprite_getBounds(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->bounds_x);
  lua_pushinteger(L, s->bounds_y);
  lua_pushinteger(L, s->bounds_w);
  lua_pushinteger(L, s->bounds_h);
  return 4;
}

static int l_sprite_getBoundsRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_createtable(L, 4, 0);
  lua_pushinteger(L, s->bounds_x);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, s->bounds_y);
  lua_rawseti(L, -2, 2);
  lua_pushinteger(L, s->bounds_w);
  lua_rawseti(L, -2, 3);
  lua_pushinteger(L, s->bounds_h);
  lua_rawseti(L, -2, 4);
  return 1;
}

static int l_sprite_setOpaque(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->opaque = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_isOpaque(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->opaque);
  return 1;
}

static int l_sprite_draw(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int x = luaL_optinteger(L, 2, s->x);
  int y = luaL_optinteger(L, 3, s->y);
  
  if (!s->visible || !s->image)
    return 0;
    
  // Use extracted frame if available
  const uint16_t *data = s->frame_data ? s->frame_data : s->image->data;
  int src_w = s->frame_data ? s->frame_w : s->width;
  int src_h = s->frame_data ? s->frame_h : s->height;
  
  // Handle NN scaling
  if (s->use_nn_scaling && s->scale_nn > 1) {
    int dst_w = src_w * s->scale_nn;
    int dst_h = src_h * s->scale_nn;
    display_draw_image_scaled_nn(x, y, data, src_w, src_h, dst_w, dst_h, s->transparent_color);
  } else if (s->rotation != 0.0f || s->scale != 1.0f || s->scale_y != 1.0f) {
    display_draw_image_scaled(x, y, src_w, src_h,
                              data, s->scale,
                              s->rotation, s->transparent_color);
  } else {
    display_draw_image_partial(x, y, src_w, src_h,
                               data, 0, 0, src_w, src_h,
                               s->flip_x, s->flip_y, s->transparent_color);
  }
  return 0;
}

static int l_sprite_updateSingle(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  if (s->updates_enabled && s->visible && s->image) {
    int draw_x = s->x;
    int draw_y = s->y;
    // Use extracted frame if available
    const uint16_t *data = s->frame_data ? s->frame_data : s->image->data;
    int src_w = s->frame_data ? s->frame_w : s->width;
    int src_h = s->frame_data ? s->frame_h : s->height;
    
    // Handle NN scaling
    if (s->use_nn_scaling && s->scale_nn > 1) {
      int dst_w = src_w * s->scale_nn;
      int dst_h = src_h * s->scale_nn;
      display_draw_image_scaled_nn(draw_x, draw_y, data, src_w, src_h, dst_w, dst_h, s->transparent_color);
    } else if (s->rotation != 0.0f || s->scale != 1.0f || s->scale_y != 1.0f) {
      display_draw_image_scaled(draw_x, draw_y, src_w, src_h,
                                data, s->scale,
                                s->rotation, s->transparent_color);
    } else {
      display_draw_image_partial(draw_x, draw_y, src_w, src_h,
                               data, 0, 0, src_w, src_h,
                               s->flip_x, s->flip_y, s->transparent_color);
    }
  }
  return 0;
}

static int l_sprite_setBackgroundDrawingCallback(lua_State *L) {
  return 0;
}

static int l_sprite_index(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  const char *key = luaL_checkstring(L, 2);

  if (!strcmp(key, "x")) {
    lua_pushinteger(L, s->x);
  } else if (!strcmp(key, "y")) {
    lua_pushinteger(L, s->y);
  } else if (!strcmp(key, "width")) {
    lua_pushinteger(L, s->width);
  } else if (!strcmp(key, "height")) {
    lua_pushinteger(L, s->height);
  } else if (!strcmp(key, "z")) {
    lua_pushinteger(L, s->z_index);
  } else if (!strcmp(key, "visible")) {
    lua_pushboolean(L, s->visible);
  } else if (!strcmp(key, "scale")) {
    lua_pushnumber(L, s->scale);
  } else if (!strcmp(key, "rotation")) {
    lua_pushnumber(L, s->rotation);
  } else if (!strcmp(key, "tag")) {
    lua_pushinteger(L, s->tag);
  } else if (!strcmp(key, "image")) {
    if (s->image) {
      lua_pushlightuserdata(L, s->image);
    } else {
      lua_pushnil(L);
    }
  } else if (!strcmp(key, "scale_nn")) {
    lua_pushinteger(L, s->scale_nn);
  } else {
    lua_getmetatable(L, 1);
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
  }
  return 1;
}

static int l_sprite_newindex(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  const char *key = luaL_checkstring(L, 2);

  if (!strcmp(key, "x")) {
    s->x = luaL_checkinteger(L, 3);
  } else if (!strcmp(key, "y")) {
    s->y = luaL_checkinteger(L, 3);
  } else if (!strcmp(key, "width")) {
    s->width = luaL_checkinteger(L, 3);
  } else if (!strcmp(key, "height")) {
    s->height = luaL_checkinteger(L, 3);
  } else if (!strcmp(key, "z")) {
    s->z_index = luaL_checkinteger(L, 3);
  } else if (!strcmp(key, "visible")) {
    s->visible = lua_toboolean(L, 3);
  } else if (!strcmp(key, "scale")) {
    s->scale = (float)luaL_checknumber(L, 3);
  } else if (!strcmp(key, "rotation")) {
    s->rotation = (float)luaL_checknumber(L, 3);
  } else if (!strcmp(key, "tag")) {
    s->tag = luaL_checkinteger(L, 3);
  }
  return 0;
}

static const luaL_Reg l_sprite_methods[] = {
    {"setImage", l_sprite_setImage},
    {"getImage", l_sprite_getImage},
    {"add", l_sprite_add},
    {"remove", l_sprite_remove},
    {"moveTo", l_sprite_moveTo},
    {"moveBy", l_sprite_moveBy},
    {"getPosition", l_sprite_getPosition},
    {"setZIndex", l_sprite_setZIndex},
    {"getZIndex", l_sprite_getZIndex},
    {"setVisible", l_sprite_setVisible},
    {"isVisible", l_sprite_isVisible},
    {"setCenter", l_sprite_setCenter},
    {"getCenter", l_sprite_getCenter},
    {"getCenterPoint", l_sprite_getCenterPoint},
    {"setSize", l_sprite_setSize},
    {"getSize", l_sprite_getSize},
    {"setScale", l_sprite_setScale},
    {"getScale", l_sprite_getScale},
    {"setScaleNN", l_sprite_setScaleNN},
    {"setTransparentColor", l_sprite_setTransparentColor},
    {"setRotation", l_sprite_setRotation},
    {"getRotation", l_sprite_getRotation},
    {"copy", l_sprite_copy},
    {"setSourceRect", l_sprite_setSourceRect},
    {"clearSourceRect", l_sprite_clearSourceRect},
    {"setUpdatesEnabled", l_sprite_setUpdatesEnabled},
    {"updatesEnabled", l_sprite_updatesEnabled},
    {"setTag", l_sprite_setTag},
    {"getTag", l_sprite_getTag},
    {"setImageDrawMode", l_sprite_setImageDrawMode},
    {"setImageFlip", l_sprite_setImageFlip},
    {"getImageFlip", l_sprite_getImageFlip},
    {"setIgnoresDrawOffset", l_sprite_setIgnoresDrawOffset},
    {"setBounds", l_sprite_setBounds},
    {"getBounds", l_sprite_getBounds},
    {"getBoundsRect", l_sprite_getBoundsRect},
    {"setOpaque", l_sprite_setOpaque},
    {"isOpaque", l_sprite_isOpaque},
    {"setBackgroundDrawingCallback", l_sprite_setBackgroundDrawingCallback},
    {"draw", l_sprite_draw},
    {"update", l_sprite_updateSingle},
    {"setCollisionsEnabled", l_sprite_setCollisionsEnabled},
    {"collisionsEnabled", l_sprite_collisionsEnabled},
    {"setCollideRect", l_sprite_setCollideRect},
    {"getCollideRect", l_sprite_getCollideRect},
    {"getCollideBounds", l_sprite_getCollideBounds},
    {"clearCollideRect", l_sprite_clearCollideRect},
    {"overlappingSprites", l_sprite_overlappingSprites},
    {"allOverlappingSprites", l_sprite_allOverlappingSprites},
    {"setGroups", l_sprite_setGroups},
    {"setCollidesWithGroups", l_sprite_setCollidesWithGroups},
    {"setGroupMask", l_sprite_setGroupMask},
    {"getGroupMask", l_sprite_getGroupMask},
    {"setCollidesWithGroupsMask", l_sprite_setCollidesWithGroupsMask},
    {"getCollidesWithGroupsMask", l_sprite_getCollidesWithGroupsMask},
    {"resetGroupMask", l_sprite_resetGroupMask},
    {"resetCollidesWithGroupsMask", l_sprite_resetCollidesWithGroupsMask},
    {"checkCollisions", l_sprite_checkCollisions},
    {"setClipRect", l_sprite_setClipRect},
    {"clearClipRect", l_sprite_clearClipRect},
    {"setAlwaysRedraw", l_sprite_setAlwaysRedraw},
    {"getAlwaysRedraw", l_sprite_getAlwaysRedraw},
    {"markDirty", l_sprite_markDirty},
    {"addDirtyRect", l_sprite_addDirtyRect},
    {"setRedrawsOnImageChange", l_sprite_setRedrawsOnImageChange},
    {NULL, NULL}};

static int l_sprite_addSprite(lua_State *L) {
  return l_sprite_add(L);
}

static int l_sprite_removeSprite(lua_State *L) {
  return l_sprite_remove(L);
}

static int l_sprite_getAllSprites(lua_State *L) {
  lua_createtable(L, s_sprite_count, 0);
  for (int i = 0; i < s_sprite_count; i++) {
    // Use light userdata: no GC finalizer, no size mismatch with GRAPHICS_SPRITE_MT.
    // Proxy full-userdata (sizeof ptr = 4 bytes) with GRAPHICS_SPRITE_MT caused
    // l_sprite_gc to read frame_data at offset ~104, corrupting the umm heap.
    lua_pushlightuserdata(L, s_sprites[i]);
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_sprite_spriteCount(lua_State *L) {
  lua_pushinteger(L, s_sprite_count);
  return 1;
}

static int l_sprite_removeAll(lua_State *L) {
  (void)L;
  s_sprite_count = 0;
  return 0;
}

static int l_sprite_removeSprites(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  int remove_count = (int)lua_rawlen(L, 1);
  
  for (int r = 1; r <= remove_count; r++) {
    lua_rawgeti(L, 1, r);
    lua_sprite_t *target = (lua_sprite_t *)luaL_checkudata(L, -1, GRAPHICS_SPRITE_MT);
    lua_pop(L, 1);
    
    for (int i = 0; i < s_sprite_count; i++) {
      if (s_sprites[i] == target) {
        for (int j = i; j < s_sprite_count - 1; j++)
          s_sprites[j] = s_sprites[j + 1];
        s_sprite_count--;
        break;
      }
    }
  }
  return 0;
}

static int l_sprite_performOnAllSprites(lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  lua_State *thread = lua_newthread(L);
  lua_xmove(L, thread, 1);
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    lua_sprite_t **ptr = (lua_sprite_t **)lua_newuserdata(thread, sizeof(lua_sprite_t *));
    *ptr = s;
    luaL_setmetatable(thread, GRAPHICS_SPRITE_MT);
    
    lua_pushvalue(thread, -1);
    if (lua_pcall(thread, 1, 0, 0) != 0) {
      lua_pop(thread, 1);
    }
  }
  lua_pop(thread, 1);
  return 0;
}

static int l_sprite_setCollisionsEnabled(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collisions_enabled = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_collisionsEnabled(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->collisions_enabled);
  return 1;
}

static int l_sprite_setCollideRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "x");
    lua_getfield(L, 2, "y");
    lua_getfield(L, 2, "w");
    lua_getfield(L, 2, "h");
    s->collide_x = luaL_optinteger(L, -4, 0);
    s->collide_y = luaL_optinteger(L, -3, 0);
    s->collide_w = luaL_optinteger(L, -2, s->width);
    s->collide_h = luaL_optinteger(L, -1, s->height);
    lua_pop(L, 4);
  } else {
    s->collide_x = luaL_checkinteger(L, 2);
    s->collide_y = luaL_checkinteger(L, 3);
    s->collide_w = luaL_checkinteger(L, 4);
    s->collide_h = luaL_checkinteger(L, 5);
  }
  return 0;
}

static int l_sprite_getCollideRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->collide_x);
  lua_pushinteger(L, s->collide_y);
  lua_pushinteger(L, s->collide_w);
  lua_pushinteger(L, s->collide_h);
  return 4;
}

static int l_sprite_getCollideBounds(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int bx = s->x + s->collide_x;
  int by = s->y + s->collide_y;
  lua_pushinteger(L, bx);
  lua_pushinteger(L, by);
  lua_pushinteger(L, s->collide_w);
  lua_pushinteger(L, s->collide_h);
  return 4;
}

static int l_sprite_clearCollideRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collide_x = 0;
  s->collide_y = 0;
  s->collide_w = s->width;
  s->collide_h = s->height;
  return 0;
}

// Returns the effective on-screen pixel dimensions of a sprite.
// Uses frame dimensions if a source rect has been extracted, otherwise the
// full image dimensions, then applies NN or bilinear scaling.
static void sprite_visual_size(const lua_sprite_t *s, int *w, int *h) {
  int bw = s->frame_w > 0 ? s->frame_w : s->width;
  int bh = s->frame_h > 0 ? s->frame_h : s->height;
  if (s->use_nn_scaling && s->scale_nn > 1) {
    *w = bw * s->scale_nn;
    *h = bh * s->scale_nn;
  } else {
    *w = (int)(bw * s->scale);
    *h = (int)(bh * s->scale_y);
  }
}

static bool spritesOverlap(lua_sprite_t *a, lua_sprite_t *b) {
  if (!a->collisions_enabled || !b->collisions_enabled) return false;

  // Use explicit collide rect if set; otherwise derive from visual size
  int aw, ah, bw, bh;
  if (a->collide_w > 0) { aw = a->collide_w; ah = a->collide_h; }
  else                  { sprite_visual_size(a, &aw, &ah); }
  if (b->collide_w > 0) { bw = b->collide_w; bh = b->collide_h; }
  else                  { sprite_visual_size(b, &bw, &bh); }

  int ax = a->x + a->collide_x;
  int ay = a->y + a->collide_y;
  int bx = b->x + b->collide_x;
  int by = b->y + b->collide_y;

  return (ax < bx + bw && ax + aw > bx &&
          ay < by + bh && ay + ah > by);
}

static int l_sprite_overlappingSprites(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *other = s_sprites[i];
    if (other != s && spritesOverlap(s, other)) {
      lua_pushlightuserdata(L, other);
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

static int l_sprite_allOverlappingSprites(lua_State *L) {
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *a = s_sprites[i];
    if (!a->collisions_enabled) continue;
    
    for (int j = i + 1; j < s_sprite_count; j++) {
      lua_sprite_t *b = s_sprites[j];
      if (!b->collisions_enabled) continue;
      
      if (spritesOverlap(a, b)) {
        lua_createtable(L, 2, 0);

        lua_pushlightuserdata(L, a);
        lua_rawseti(L, -2, 1);

        lua_pushlightuserdata(L, b);
        lua_rawseti(L, -2, 2);

        lua_rawseti(L, -2, ++count);
      }
    }
  }
  return 1;
}

static int l_sprite_setGroups(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->group_mask = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_setCollidesWithGroups(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collides_with_mask = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_setGroupMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->group_mask = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_getGroupMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->group_mask);
  return 1;
}

static int l_sprite_setCollidesWithGroupsMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collides_with_mask = luaL_checkinteger(L, 2);
  return 0;
}

static int l_sprite_getCollidesWithGroupsMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushinteger(L, s->collides_with_mask);
  return 1;
}

static int l_sprite_resetGroupMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->group_mask = 0;
  return 0;
}

static int l_sprite_resetCollidesWithGroupsMask(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->collides_with_mask = 0;
  return 0;
}

static int l_sprite_checkCollisions(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  int px, py;
  
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "x");
    lua_getfield(L, 2, "y");
    px = luaL_optinteger(L, -2, 0);
    py = luaL_optinteger(L, -1, 0);
    lua_pop(L, 2);
  } else {
    px = luaL_checkinteger(L, 2);
    py = luaL_checkinteger(L, 3);
  }
  
  int sx = s->x + s->collide_x;
  int sy = s->y + s->collide_y;
  
  bool hit = (px >= sx && px < sx + s->collide_w &&
              py >= sy && py < sy + s->collide_h);
  lua_pushboolean(L, hit);
  return 1;
}

static int l_sprite_querySpritesAtPoint(lua_State *L) {
  int px, py;
  
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "x");
    lua_getfield(L, 1, "y");
    px = luaL_optinteger(L, -2, 0);
    py = luaL_optinteger(L, -1, 0);
    lua_pop(L, 2);
  } else {
    px = luaL_checkinteger(L, 1);
    py = luaL_checkinteger(L, 2);
  }
  
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    int sx = s->x + s->collide_x;
    int sy = s->y + s->collide_y;
    
    if (px >= sx && px < sx + s->collide_w &&
        py >= sy && py < sy + s->collide_h) {
      lua_sprite_t **ptr = (lua_sprite_t **)lua_newuserdata(L, sizeof(lua_sprite_t *));
      *ptr = s;
      luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

static int l_sprite_querySpritesInRect(lua_State *L) {
  int rx, ry, rw, rh;
  
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "x");
    lua_getfield(L, 1, "y");
    lua_getfield(L, 1, "w");
    lua_getfield(L, 1, "h");
    rx = luaL_optinteger(L, -4, 0);
    ry = luaL_optinteger(L, -3, 0);
    rw = luaL_optinteger(L, -2, 320);
    rh = luaL_optinteger(L, -1, 320);
    lua_pop(L, 4);
  } else {
    rx = luaL_checkinteger(L, 1);
    ry = luaL_checkinteger(L, 2);
    rw = luaL_checkinteger(L, 3);
    rh = luaL_checkinteger(L, 4);
  }
  
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    int sx = s->x + s->collide_x;
    int sy = s->y + s->collide_y;
    
    if (sx < rx + rw && sx + s->collide_w > rx &&
        sy < ry + rh && sy + s->collide_h > ry) {
      lua_sprite_t **ptr = (lua_sprite_t **)lua_newuserdata(L, sizeof(lua_sprite_t *));
      *ptr = s;
      luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

static int l_sprite_setClipRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "x");
    lua_getfield(L, 2, "y");
    lua_getfield(L, 2, "w");
    lua_getfield(L, 2, "h");
    s->clip_x = luaL_optinteger(L, -4, 0);
    s->clip_y = luaL_optinteger(L, -3, 0);
    s->clip_w = luaL_optinteger(L, -2, s->width);
    s->clip_h = luaL_optinteger(L, -1, s->height);
    lua_pop(L, 4);
    s->has_clip = true;
  } else if (lua_gettop(L) >= 5) {
    s->clip_x = luaL_checkinteger(L, 2);
    s->clip_y = luaL_checkinteger(L, 3);
    s->clip_w = luaL_checkinteger(L, 4);
    s->clip_h = luaL_checkinteger(L, 5);
    s->has_clip = true;
  } else {
    s->has_clip = false;
  }
  return 0;
}

static int l_sprite_clearClipRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->has_clip = false;
  return 0;
}

static int l_sprite_setAlwaysRedraw(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->always_redraw = lua_toboolean(L, 2);
  return 0;
}

static int l_sprite_getAlwaysRedraw(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  lua_pushboolean(L, s->always_redraw);
  return 1;
}

static int l_sprite_markDirty(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->dirty = true;
  return 0;
}

static int l_sprite_addDirtyRect(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  (void)s;
  return 0;
}

static int l_sprite_setRedrawsOnImageChange(lua_State *L) {
  lua_sprite_t *s = check_sprite(L, 1);
  s->redraws_on_image_change = lua_toboolean(L, 2);
  return 0;
}

static bool sprite_line_line_intersect(int x1, int y1, int x2, int y2,
                                       int x3, int y3, int x4, int y4) {
  int denom = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
  if (denom == 0) return false;
  
  float t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / (float)denom;
  float u = -((x1 - x2) * (y1 - y3) - (y1 - y2) * (x1 - x3)) / (float)denom;
  
  return (t >= 0 && t <= 1 && u >= 0 && u <= 1);
}

static bool sprite_line_intersect(int x1, int y1, int x2, int y2, 
                                  int rx, int ry, int rw, int rh) {
  if (rw <= 0 || rh <= 0) return false;
  
  int left = rx;
  int right = rx + rw;
  int top = ry;
  int bottom = ry + rh;
  
  if ((x1 >= left && x1 <= right && y1 >= top && y1 <= bottom) ||
      (x2 >= left && x2 <= right && y2 >= top && y2 <= bottom)) {
    return true;
  }
  
  if (sprite_line_line_intersect(x1, y1, x2, y2, left, top, right, top)) return true;
  if (sprite_line_line_intersect(x1, y1, x2, y2, left, bottom, right, bottom)) return true;
  if (sprite_line_line_intersect(x1, y1, x2, y2, left, top, left, bottom)) return true;
  if (sprite_line_line_intersect(x1, y1, x2, y2, right, top, right, bottom)) return true;
  
  return false;
}

static int l_sprite_querySpritesAlongLine(lua_State *L) {
  int x1 = luaL_checkinteger(L, 1);
  int y1 = luaL_checkinteger(L, 2);
  int x2 = luaL_checkinteger(L, 3);
  int y2 = luaL_checkinteger(L, 4);
  
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    int sx = s->x;
    int sy = s->y;
    int sw = s->width > 0 ? s->width : s->collide_w;
    int sh = s->height > 0 ? s->height : s->collide_h;
    
    if (sprite_line_intersect(x1, y1, x2, y2, sx, sy, sw, sh)) {
      lua_sprite_t **ptr = (lua_sprite_t **)lua_newuserdata(L, sizeof(lua_sprite_t *));
      *ptr = s;
      luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

static bool sprite_line_rect_intersection(int x1, int y1, int x2, int y2,
                                           int rx, int ry, int rw, int rh,
                                           int *out_x, int *out_y) {
  if (rw <= 0 || rh <= 0) return false;
  
  int left = rx;
  int right = rx + rw;
  int top = ry;
  int bottom = ry + rh;
  
  int ix = 0, iy = 0;
  bool found = false;
  float min_dist = 1e9f;
  
  int edges[4][4] = {
    {left, top, right, top},
    {left, bottom, right, bottom},
    {left, top, left, bottom},
    {right, top, right, bottom}
  };
  
  for (int i = 0; i < 4; i++) {
    int ex1 = edges[i][0], ey1 = edges[i][1], ex2 = edges[i][2], ey2 = edges[i][3];
    int denom = (x1 - x2) * (ey1 - ey2) - (y1 - y2) * (ex1 - ex2);
    if (denom == 0) continue;
    float t = ((x1 - ex1) * (ey1 - ey2) - (y1 - ey1) * (ex1 - ex2)) / (float)denom;
    float u = -((x1 - x2) * (y1 - ey1) - (y1 - y2) * (x1 - ex1)) / (float)denom;
    if (t >= 0 && t <= 1 && u >= 0 && u <= 1) {
      int px = (int)(x1 + t * (x2 - x1));
      int py = (int)(y1 + t * (y2 - y1));
      float d = (px - x1) * (px - x1) + (py - y1) * (py - y1);
      if (d < min_dist) { min_dist = d; ix = px; iy = py; found = true; }
    }
  }
  
  if (found) {
    *out_x = ix;
    *out_y = iy;
  }
  return found;
}

static int l_sprite_querySpriteInfoAlongLine(lua_State *L) {
  int x1 = luaL_checkinteger(L, 1);
  int y1 = luaL_checkinteger(L, 2);
  int x2 = luaL_checkinteger(L, 3);
  int y2 = luaL_checkinteger(L, 4);
  
  lua_createtable(L, 0, 0);
  int count = 0;
  
  for (int i = 0; i < s_sprite_count; i++) {
    lua_sprite_t *s = s_sprites[i];
    int sx = s->x;
    int sy = s->y;
    int sw = s->width > 0 ? s->width : s->collide_w;
    int sh = s->height > 0 ? s->height : s->collide_h;
    
    int ix, iy;
    if (sprite_line_rect_intersection(x1, y1, x2, y2, sx, sy, sw, sh, &ix, &iy)) {
      lua_createtable(L, 0, 0);
      lua_sprite_t **ptr = (lua_sprite_t **)lua_newuserdata(L, sizeof(lua_sprite_t *));
      *ptr = s;
      luaL_setmetatable(L, GRAPHICS_SPRITE_MT);
      lua_setfield(L, -2, "sprite");
      lua_pushinteger(L, ix);
      lua_setfield(L, -2, "x");
      lua_pushinteger(L, iy);
      lua_setfield(L, -2, "y");
      lua_rawseti(L, -2, ++count);
    }
  }
  return 1;
}

static const luaL_Reg l_sprite_lib[] = {
    {"new", l_sprite_new},
    {"addSprite", l_sprite_addSprite},
    {"removeSprite", l_sprite_removeSprite},
    {"update", l_sprite_update},
    {"getAllSprites", l_sprite_getAllSprites},
    {"spriteCount", l_sprite_spriteCount},
    {"removeAll", l_sprite_removeAll},
    {"removeSprites", l_sprite_removeSprites},
    {"performOnAllSprites", l_sprite_performOnAllSprites},
    {"querySpritesAtPoint", l_sprite_querySpritesAtPoint},
    {"querySpritesInRect", l_sprite_querySpritesInRect},
    {"querySpritesAlongLine", l_sprite_querySpritesAlongLine},
    {"querySpriteInfoAlongLine", l_sprite_querySpriteInfoAlongLine},
    {NULL, NULL}};

// ── Spritesheet System ─────────────────────────────────────────────────────────

#define GRAPHICS_SPRITESHEET_MT "picocalc.graphics.spritesheet"
#define MAX_FRAMES 64

typedef struct {
  lua_image_t *image;
  int frame_count;
  int frame_x[MAX_FRAMES];
  int frame_y[MAX_FRAMES];
  int frame_w[MAX_FRAMES];
  int frame_h[MAX_FRAMES];
} lua_spritesheet_t;

static lua_spritesheet_t *check_spritesheet(lua_State *L, int idx) {
  return (lua_spritesheet_t *)luaL_checkudata(L, idx, GRAPHICS_SPRITESHEET_MT);
}

static int l_spritesheet_gc(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  ss->image = NULL;
  return 0;
}

static int l_spritesheet_new(lua_State *L) {
  lua_image_t *img = NULL;
  if (lua_isuserdata(L, 1)) {
    img = (lua_image_t *)luaL_checkudata(L, 1, GRAPHICS_IMAGE_MT);
  }
  
  lua_spritesheet_t *ss = (lua_spritesheet_t *)lua_newuserdata(L, sizeof(lua_spritesheet_t));
  ss->image = img;
  ss->frame_count = 0;
  
  luaL_setmetatable(L, GRAPHICS_SPRITESHEET_MT);
  return 1;
}

static int l_spritesheet_newGrid(lua_State *L) {
  lua_image_t *img = (lua_image_t *)luaL_checkudata(L, 1, GRAPHICS_IMAGE_MT);
  int cols = luaL_checkinteger(L, 2);
  int rows = luaL_checkinteger(L, 3);
  int frame_w = luaL_checkinteger(L, 4);
  int frame_h = luaL_checkinteger(L, 5);
  
  lua_spritesheet_t *ss = (lua_spritesheet_t *)lua_newuserdata(L, sizeof(lua_spritesheet_t));
  ss->image = img;
  ss->frame_count = 0;
  
  int x = 0, y = 0;
  for (int r = 0; r < rows && ss->frame_count < MAX_FRAMES; r++) {
    for (int c = 0; c < cols && ss->frame_count < MAX_FRAMES; c++) {
      ss->frame_x[ss->frame_count] = x;
      ss->frame_y[ss->frame_count] = y;
      ss->frame_w[ss->frame_count] = frame_w;
      ss->frame_h[ss->frame_count] = frame_h;
      ss->frame_count++;
      x += frame_w;
    }
    x = 0;
    y += frame_h;
  }
  
  luaL_setmetatable(L, GRAPHICS_SPRITESHEET_MT);
  return 1;
}

static int l_spritesheet_addFrame(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  if (ss->frame_count >= MAX_FRAMES)
    return luaL_error(L, "max frames reached");
  
  int idx = ss->frame_count;
  ss->frame_x[idx] = luaL_checkinteger(L, 2);
  ss->frame_y[idx] = luaL_checkinteger(L, 3);
  ss->frame_w[idx] = luaL_checkinteger(L, 4);
  ss->frame_h[idx] = luaL_checkinteger(L, 5);
  ss->frame_count++;
  
  lua_pushinteger(L, idx);
  return 1;
}

static int l_spritesheet_getFrameCount(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  lua_pushinteger(L, ss->frame_count);
  return 1;
}

static int l_spritesheet_getFrame(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  int idx = luaL_checkinteger(L, 2);
  if (idx < 0 || idx >= ss->frame_count)
    return 0;
  
  lua_createtable(L, 4, 0);
  lua_pushinteger(L, ss->frame_x[idx]);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, ss->frame_y[idx]);
  lua_rawseti(L, -2, 2);
  lua_pushinteger(L, ss->frame_w[idx]);
  lua_rawseti(L, -2, 3);
  lua_pushinteger(L, ss->frame_h[idx]);
  lua_rawseti(L, -2, 4);
  return 1;
}

static int l_spritesheet_getImage(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  if (!ss->image) return 0;
  lua_pushlightuserdata(L, ss->image);
  return 1;
}

static int l_spritesheet_drawFrame(lua_State *L) {
  lua_spritesheet_t *ss = check_spritesheet(L, 1);
  int frame_idx = luaL_checkinteger(L, 2);
  int x = luaL_checkinteger(L, 3);
  int y = luaL_checkinteger(L, 4);
  
  if (!ss->image || frame_idx < 0 || frame_idx >= ss->frame_count)
    return 0;
  
  bool flip = lua_toboolean(L, 5);
  
  display_draw_image_partial(x, y, ss->image->w, ss->image->h,
                            ss->image->data,
                            ss->frame_x[frame_idx], ss->frame_y[frame_idx],
                            ss->frame_w[frame_idx], ss->frame_h[frame_idx],
                            flip, false, 0);
  return 0;
}

static const luaL_Reg l_spritesheet_methods[] = {
    {"addFrame", l_spritesheet_addFrame},
    {"getFrameCount", l_spritesheet_getFrameCount},
    {"getFrame", l_spritesheet_getFrame},
    {"getImage", l_spritesheet_getImage},
    {"drawFrame", l_spritesheet_drawFrame},
    {NULL, NULL}};

static const luaL_Reg l_spritesheet_lib[] = {
    {"new", l_spritesheet_new},
    {"newGrid", l_spritesheet_newGrid},
    {NULL, NULL}};

static const luaL_Reg l_graphics_lib[] = {
    {"setColor", l_graphics_setColor},
    {"setBackgroundColor", l_graphics_setBackgroundColor},
    {"setTransparentColor", l_graphics_setTransparentColor},
    {"getTransparentColor", l_graphics_getTransparentColor},
    {"clear", l_graphics_clear},
    {"drawGrid", l_graphics_drawGrid},
    {"fillBorderedRect", l_graphics_fillBorderedRect},
    {"updateDrawParticles", l_graphics_updateDrawParticles},
    {"draw3DWireframe", l_graphics_draw3DWireframe},
    {NULL, NULL}};

// ── Animation System ─────────────────────────────────────────────────────────

#define GRAPHICS_ANIMATION_LOOP_MT "picocalc.graphics.animation.loop"
#define MAX_ANIMATION_LOOP_FRAMES 32

typedef struct {
  lua_image_t *frames[MAX_ANIMATION_LOOP_FRAMES];
  int frame_count;
  int current_frame;
  uint32_t interval_ms;
  uint32_t last_update_ms;
  bool looping;
  bool valid;
} lua_animation_loop_t;

static lua_animation_loop_t *check_animation_loop(lua_State *L, int idx) {
  return (lua_animation_loop_t *)luaL_checkudata(L, idx, GRAPHICS_ANIMATION_LOOP_MT);
}

static int l_animation_loop_gc(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  loop->frame_count = 0;
  loop->valid = false;
  return 0;
}

static int l_animation_loop_new(lua_State *L) {
  lua_animation_loop_t *loop = (lua_animation_loop_t *)lua_newuserdata(L, sizeof(lua_animation_loop_t));
  loop->frame_count = 0;
  loop->current_frame = 0;
  loop->interval_ms = 100;
  loop->last_update_ms = to_ms_since_boot(get_absolute_time());
  loop->looping = true;
  loop->valid = false;

  if (lua_gettop(L) >= 1) {
    if (lua_isnumber(L, 1)) {
      loop->interval_ms = luaL_checkinteger(L, 1);
    }
  }

  if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
    lua_pushvalue(L, 2);
    loop->frame_count = (int)lua_rawlen(L, -1);
    if (loop->frame_count > MAX_ANIMATION_LOOP_FRAMES) {
      loop->frame_count = MAX_ANIMATION_LOOP_FRAMES;
    }
    for (int i = 0; i < loop->frame_count; i++) {
      lua_rawgeti(L, -1, i + 1);
      if (lua_isuserdata(L, -1)) {
        loop->frames[i] = (lua_image_t *)lua_touserdata(L, -1);
      } else {
        loop->frames[i] = NULL;
      }
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
    loop->valid = (loop->frame_count > 0);
  }

  if (lua_gettop(L) >= 3) {
    loop->looping = lua_toboolean(L, 3);
  }

  luaL_setmetatable(L, GRAPHICS_ANIMATION_LOOP_MT);
  return 1;
}

static int l_animation_loop_draw(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);
  bool flip = lua_toboolean(L, 4);

  if (!loop->valid || !loop->frames[loop->current_frame])
    return 0;

  lua_image_t *img = loop->frames[loop->current_frame];
  display_draw_image_partial(x, y, img->w, img->h, img->data,
                            0, 0, img->w, img->h, flip, false, 0);
  return 0;
}

static int l_animation_loop_update(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  uint32_t now = to_ms_since_boot(get_absolute_time());

  if (!loop->valid)
    return 0;

  if (now - loop->last_update_ms >= loop->interval_ms) {
    loop->last_update_ms = now;
    loop->current_frame++;
    if (loop->current_frame >= loop->frame_count) {
      if (loop->looping) {
        loop->current_frame = 0;
      } else {
        loop->current_frame = loop->frame_count - 1;
      }
    }
  }
  return 0;
}

static int l_animation_loop_image(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  if (!loop->valid || !loop->frames[loop->current_frame])
    return 0;
  lua_pushlightuserdata(L, loop->frames[loop->current_frame]);
  return 1;
}

static int l_animation_loop_isValid(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  lua_pushboolean(L, loop->valid);
  return 1;
}

static int l_animation_loop_getFrameIndex(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  lua_pushinteger(L, loop->current_frame);
  return 1;
}

static int l_animation_loop_setImageTable(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  loop->frame_count = 0;
  loop->current_frame = 0;
  loop->valid = false;

  if (!lua_istable(L, 2))
    return 0;

  loop->frame_count = (int)lua_rawlen(L, 2);
  if (loop->frame_count > MAX_ANIMATION_LOOP_FRAMES) {
    loop->frame_count = MAX_ANIMATION_LOOP_FRAMES;
  }
  for (int i = 0; i < loop->frame_count; i++) {
    lua_rawgeti(L, 2, i + 1);
    if (lua_isuserdata(L, -1)) {
      loop->frames[i] = (lua_image_t *)lua_touserdata(L, -1);
    } else {
      loop->frames[i] = NULL;
    }
    lua_pop(L, 1);
  }
  loop->valid = (loop->frame_count > 0);
  loop->last_update_ms = to_ms_since_boot(get_absolute_time());
  return 0;
}

static int l_animation_loop_setInterval(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  loop->interval_ms = luaL_checkinteger(L, 2);
  return 0;
}

static int l_animation_loop_setLooping(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  loop->looping = lua_toboolean(L, 2);
  return 0;
}

static int l_animation_loop_reset(lua_State *L) {
  lua_animation_loop_t *loop = check_animation_loop(L, 1);
  loop->current_frame = 0;
  loop->last_update_ms = to_ms_since_boot(get_absolute_time());
  return 0;
}

static const luaL_Reg l_animation_loop_methods[] = {
    {"draw", l_animation_loop_draw},
    {"update", l_animation_loop_update},
    {"image", l_animation_loop_image},
    {"isValid", l_animation_loop_isValid},
    {"getFrameIndex", l_animation_loop_getFrameIndex},
    {"setImageTable", l_animation_loop_setImageTable},
    {"setInterval", l_animation_loop_setInterval},
    {"setLooping", l_animation_loop_setLooping},
    {"reset", l_animation_loop_reset},
    {NULL, NULL}};

static const luaL_Reg l_animation_loop_lib[] = {
    {"new", l_animation_loop_new},
    {NULL, NULL}};

// ── Easing Functions ─────────────────────────────────────────────────────────

static float easing_linear(float t) {
  return t;
}

static float easing_sineIn(float t) {
  return sinf(t * (float)M_PI_2);
}

static float easing_sineOut(float t) {
  return 1.0f - sinf((1.0f - t) * (float)M_PI_2);
}

static float easing_sineInOut(float t) {
  return -(cosf((float)M_PI * t) - 1.0f) / 2.0f;
}

static float easing_quadIn(float t) {
  return t * t;
}

static float easing_quadOut(float t) {
  return 1.0f - (1.0f - t) * (1.0f - t);
}

static float easing_quadInOut(float t) {
  return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) / 2.0f;
}

static float easing_cubicIn(float t) {
  return t * t * t;
}

static float easing_cubicOut(float t) {
  return 1.0f - powf(1.0f - t, 3.0f);
}

static float easing_cubicInOut(float t) {
  return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

typedef float (*easing_fn)(float);

static easing_fn get_easing_fn(const char *name) {
  if (!strcmp(name, "linear")) return easing_linear;
  if (!strcmp(name, "sineIn") || !strcmp(name, "SineIn")) return easing_sineIn;
  if (!strcmp(name, "sineOut") || !strcmp(name, "SineOut")) return easing_sineOut;
  if (!strcmp(name, "sineInOut") || !strcmp(name, "SineInOut")) return easing_sineInOut;
  if (!strcmp(name, "quadIn") || !strcmp(name, "QuadIn")) return easing_quadIn;
  if (!strcmp(name, "quadOut") || !strcmp(name, "QuadOut")) return easing_quadOut;
  if (!strcmp(name, "quadInOut") || !strcmp(name, "QuadInOut")) return easing_quadInOut;
  if (!strcmp(name, "cubicIn") || !strcmp(name, "CubicIn")) return easing_cubicIn;
  if (!strcmp(name, "cubicOut") || !strcmp(name, "CubicOut")) return easing_cubicOut;
  if (!strcmp(name, "cubicInOut") || !strcmp(name, "CubicInOut")) return easing_cubicInOut;
  return easing_linear;
}

// ── Animator ─────────────────────────────────────────────────────────────────

#define GRAPHICS_ANIMATOR_MT "picocalc.graphics.animator"

typedef struct {
  float start_value;
  float end_value;
  uint32_t duration_ms;
  uint32_t start_time_ms;
  float easing_amplitude;
  float easing_period;
  int repeat_count;
  int current_repeat;
  bool reverses;
  bool ended;
  easing_fn easing;
} lua_animator_t;

static lua_animator_t *check_animator(lua_State *L, int idx) {
  return (lua_animator_t *)luaL_checkudata(L, idx, GRAPHICS_ANIMATOR_MT);
}

static int l_animator_gc(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  a->ended = true;
  return 0;
}

static int l_animator_new(lua_State *L) {
  lua_animator_t *a = (lua_animator_t *)lua_newuserdata(L, sizeof(lua_animator_t));
  a->duration_ms = luaL_checkinteger(L, 1);
  a->start_value = (float)luaL_checknumber(L, 2);
  a->end_value = (float)luaL_checknumber(L, 3);
  a->start_time_ms = to_ms_since_boot(get_absolute_time());
  a->easing_amplitude = 1.0f;
  a->easing_period = 0.0f;
  a->repeat_count = 1;
  a->current_repeat = 0;
  a->reverses = false;
  a->ended = false;
  a->easing = easing_linear;

  if (lua_gettop(L) >= 4 && lua_isstring(L, 4)) {
    a->easing = get_easing_fn(luaL_checkstring(L, 4));
  }

  if (lua_gettop(L) >= 5) {
    a->start_time_ms += luaL_checkinteger(L, 5);
  }

  luaL_setmetatable(L, GRAPHICS_ANIMATOR_MT);
  return 1;
}

static int l_animator_currentValue(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  if (a->ended) {
    lua_pushnumber(L, a->reverses ? a->start_value : a->end_value);
    return 1;
  }

  uint32_t now = to_ms_since_boot(get_absolute_time());
  uint32_t elapsed = now - a->start_time_ms;
  float t = (float)elapsed / (float)a->duration_ms;

  if (t >= 1.0f) {
    if (a->current_repeat < a->repeat_count - 1) {
      a->current_repeat++;
      a->start_time_ms = now;
      t = 0.0f;
    } else if (a->reverses) {
      float tmp = a->start_value;
      a->start_value = a->end_value;
      a->end_value = tmp;
      a->start_time_ms = now;
      a->reverses = false;
      t = 0.0f;
    } else {
      t = 1.0f;
      a->ended = true;
    }
  }

  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  float eased = a->easing(t);
  float value = a->start_value + (a->end_value - a->start_value) * eased;
  lua_pushnumber(L, value);
  return 1;
}

static int l_animator_valueAtTime(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  uint32_t time_ms = luaL_checkinteger(L, 2);

  float t = (float)time_ms / (float)a->duration_ms;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  float eased = a->easing(t);
  float value = a->start_value + (a->end_value - a->start_value) * eased;
  lua_pushnumber(L, value);
  return 1;
}

static int l_animator_progress(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  if (a->ended) {
    lua_pushnumber(L, 1.0f);
    return 1;
  }

  uint32_t now = to_ms_since_boot(get_absolute_time());
  uint32_t elapsed = now - a->start_time_ms;
  float progress = (float)elapsed / (float)a->duration_ms;
  if (progress > 1.0f) progress = 1.0f;
  lua_pushnumber(L, progress);
  return 1;
}

static int l_animator_reset(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  a->start_time_ms = to_ms_since_boot(get_absolute_time());
  a->ended = false;
  a->current_repeat = 0;

  if (lua_isnumber(L, 2)) {
    a->duration_ms = luaL_checkinteger(L, 2);
  }
  return 0;
}

static int l_animator_ended(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  lua_pushboolean(L, a->ended);
  return 1;
}

static int l_animator_index(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  const char *key = luaL_checkstring(L, 2);

  if (!strcmp(key, "easingAmplitude")) {
    lua_pushnumber(L, a->easing_amplitude);
  } else if (!strcmp(key, "easingPeriod")) {
    lua_pushnumber(L, a->easing_period);
  } else if (!strcmp(key, "repeatCount")) {
    lua_pushinteger(L, a->repeat_count);
  } else if (!strcmp(key, "reverses")) {
    lua_pushboolean(L, a->reverses);
  } else {
    lua_getmetatable(L, 1);
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
  }
  return 1;
}

static int l_animator_newindex(lua_State *L) {
  lua_animator_t *a = check_animator(L, 1);
  const char *key = luaL_checkstring(L, 2);

  if (!strcmp(key, "easingAmplitude")) {
    a->easing_amplitude = (float)luaL_checknumber(L, 3);
  } else if (!strcmp(key, "easingPeriod")) {
    a->easing_period = (float)luaL_checknumber(L, 3);
  } else if (!strcmp(key, "repeatCount")) {
    a->repeat_count = luaL_checkinteger(L, 3);
  } else if (!strcmp(key, "reverses")) {
    a->reverses = lua_toboolean(L, 3);
  }
  return 0;
}

static const luaL_Reg l_animator_methods[] = {
    {"currentValue", l_animator_currentValue},
    {"valueAtTime", l_animator_valueAtTime},
    {"progress", l_animator_progress},
    {"reset", l_animator_reset},
    {"ended", l_animator_ended},
    {NULL, NULL}};

static const luaL_Reg l_animator_lib[] = {
    {"new", l_animator_new},
    {NULL, NULL}};

// ── Blinker ─────────────────────────────────────────────────────────────────

#define GRAPHICS_ANIMATION_BLINKER_MT "picocalc.graphics.animation.blinker"
#define MAX_BLINKERS 32

typedef struct {
  uint32_t on_duration_ms;
  uint32_t off_duration_ms;
  bool loop;
  int cycles;
  int current_cycle;
  uint32_t start_time_ms;
  bool running;
  bool state;
} lua_animation_blinker_t;

static lua_animation_blinker_t *s_blinkers[MAX_BLINKERS];
static int s_blinker_count = 0;

static lua_animation_blinker_t *check_blinker(lua_State *L, int idx) {
  return (lua_animation_blinker_t *)luaL_checkudata(L, idx, GRAPHICS_ANIMATION_BLINKER_MT);
}

static int l_animation_blinker_gc(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);
  b->running = false;
  return 0;
}

static int l_animation_blinker_new(lua_State *L) {
  lua_animation_blinker_t *b = (lua_animation_blinker_t *)lua_newuserdata(L, sizeof(lua_animation_blinker_t));
  b->on_duration_ms = 500;
  b->off_duration_ms = 500;
  b->loop = true;
  b->cycles = 0;
  b->current_cycle = 0;
  b->start_time_ms = to_ms_since_boot(get_absolute_time());
  b->running = false;
  b->state = true;

  int top = lua_gettop(L);
  if (top >= 1) b->on_duration_ms = luaL_checkinteger(L, 1);
  if (top >= 2) b->off_duration_ms = luaL_checkinteger(L, 2);
  if (top >= 3) b->loop = lua_toboolean(L, 3);
  if (top >= 4) b->cycles = luaL_checkinteger(L, 4);
  if (top >= 5) b->state = !lua_toboolean(L, 5);

  if (s_blinker_count < MAX_BLINKERS) {
    s_blinkers[s_blinker_count++] = b;
  }

  luaL_setmetatable(L, GRAPHICS_ANIMATION_BLINKER_MT);
  return 1;
}

static int l_animation_blinker_update(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);

  if (!b->running) {
    b->running = true;
    b->start_time_ms = to_ms_since_boot(get_absolute_time());
    b->current_cycle = 0;
    b->state = true;
    lua_pushboolean(L, b->state);
    return 1;
  }

  uint32_t now = to_ms_since_boot(get_absolute_time());
  uint32_t elapsed = now - b->start_time_ms;
  uint32_t cycle_time = b->on_duration_ms + b->off_duration_ms;

  if (cycle_time == 0) {
    lua_pushboolean(L, b->state);
    return 1;
  }

  uint32_t cycle_elapsed = elapsed % cycle_time;
  bool new_state = (cycle_elapsed < b->on_duration_ms);

  if (new_state != b->state) {
    b->state = new_state;
  }

  if (!b->loop && b->cycles > 0) {
    uint32_t total_cycles = elapsed / cycle_time;
    if (total_cycles >= (uint32_t)b->cycles) {
      b->running = false;
      b->state = false;
    }
  }

  lua_pushboolean(L, b->running ? b->state : false);
  return 1;
}

static int l_animation_blinker_start(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);

  int top = lua_gettop(L);
  if (top >= 1) b->on_duration_ms = luaL_checkinteger(L, 1);
  if (top >= 2) b->off_duration_ms = luaL_checkinteger(L, 2);
  if (top >= 3) b->loop = lua_toboolean(L, 3);
  if (top >= 4) b->cycles = luaL_checkinteger(L, 4);
  if (top >= 5) b->state = !lua_toboolean(L, 5);

  b->start_time_ms = to_ms_since_boot(get_absolute_time());
  b->current_cycle = 0;
  b->running = true;
  b->state = true;
  return 0;
}

static int l_animation_blinker_startLoop(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);
  b->loop = true;
  b->cycles = 0;
  b->start_time_ms = to_ms_since_boot(get_absolute_time());
  b->current_cycle = 0;
  b->running = true;
  b->state = true;
  return 0;
}

static int l_animation_blinker_stop(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);
  b->running = false;
  return 0;
}

static int l_animation_blinker_remove(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);
  b->running = false;

  for (int i = 0; i < s_blinker_count; i++) {
    if (s_blinkers[i] == b) {
      for (int j = i; j < s_blinker_count - 1; j++) {
        s_blinkers[j] = s_blinkers[j + 1];
      }
      s_blinker_count--;
      break;
    }
  }
  return 0;
}

static int l_animation_blinker_updateAll(lua_State *L) {
  uint32_t now = to_ms_since_boot(get_absolute_time());

  for (int i = 0; i < s_blinker_count; i++) {
    lua_animation_blinker_t *b = s_blinkers[i];
    if (!b->running) continue;

    uint32_t elapsed = now - b->start_time_ms;
    uint32_t cycle_time = b->on_duration_ms + b->off_duration_ms;

    if (cycle_time == 0) continue;

    uint32_t cycle_elapsed = elapsed % cycle_time;
    bool new_state = (cycle_elapsed < b->on_duration_ms);

    if (new_state != b->state) {
      b->state = new_state;
    }

    if (!b->loop && b->cycles > 0) {
      uint32_t total_cycles = elapsed / cycle_time;
      if (total_cycles >= (uint32_t)b->cycles) {
        b->running = false;
        b->state = false;
      }
    }
  }
  return 0;
}

static int l_animation_blinker_stopAll(lua_State *L) {
  (void)L;
  for (int i = 0; i < s_blinker_count; i++) {
    s_blinkers[i]->running = false;
  }
  return 0;
}

static int l_animation_blinker_isRunning(lua_State *L) {
  lua_animation_blinker_t *b = check_blinker(L, 1);
  lua_pushboolean(L, b->running);
  return 1;
}

static const luaL_Reg l_animation_blinker_methods[] = {
    {"update", l_animation_blinker_update},
    {"start", l_animation_blinker_start},
    {"startLoop", l_animation_blinker_startLoop},
    {"stop", l_animation_blinker_stop},
    {"remove", l_animation_blinker_remove},
    {"isRunning", l_animation_blinker_isRunning},
    {NULL, NULL}};

static const luaL_Reg l_animation_blinker_lib[] = {
    {"new", l_animation_blinker_new},
    {"updateAll", l_animation_blinker_updateAll},
    {"stopAll", l_animation_blinker_stopAll},
    {NULL, NULL}};

void lua_bridge_graphics_init(lua_State *L) {
  s_sprite_count = 0;  // reset on each app launch
  s_blinker_count = 0;  // reset blinkers on each app launch

  // Install Graphics Image metatable
  luaL_newmetatable(L, GRAPHICS_IMAGE_MT);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, l_graphics_image_methods, 0);
  lua_pushcfunction(L, l_graphics_image_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  // Install Graphics Image Stream metatable
  luaL_newmetatable(L, GRAPHICS_IMAGESTREAM_MT);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, l_graphics_imagestream_methods, 0);
  lua_pushcfunction(L, l_graphics_imagestream_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  // Install Graphics Sprite metatable
  luaL_newmetatable(L, GRAPHICS_SPRITE_MT);
  lua_pushcfunction(L, l_sprite_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, l_sprite_newindex);
  lua_setfield(L, -2, "__newindex");
  luaL_setfuncs(L, l_sprite_methods, 0);
  lua_pushcfunction(L, l_sprite_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  // Install Graphics Spritesheet metatable
  luaL_newmetatable(L, GRAPHICS_SPRITESHEET_MT);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, l_spritesheet_methods, 0);
  lua_pushcfunction(L, l_spritesheet_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  // Install Animation Loop metatable
  luaL_newmetatable(L, GRAPHICS_ANIMATION_LOOP_MT);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, l_animation_loop_methods, 0);
  lua_pushcfunction(L, l_animation_loop_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  // Install Animator metatable
  luaL_newmetatable(L, GRAPHICS_ANIMATOR_MT);
  lua_pushcfunction(L, l_animator_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, l_animator_newindex);
  lua_setfield(L, -2, "__newindex");
  luaL_setfuncs(L, l_animator_methods, 0);
  lua_pushcfunction(L, l_animator_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  // Install Animation Blinker metatable
  luaL_newmetatable(L, GRAPHICS_ANIMATION_BLINKER_MT);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, l_animation_blinker_methods, 0);
  lua_pushcfunction(L, l_animation_blinker_gc);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);

  // Build picocalc.graphics table
  lua_newtable(L);
  luaL_setfuncs(L, l_graphics_lib, 0);

  lua_newtable(L);
  luaL_setfuncs(L, l_graphics_image_lib, 0);
  lua_setfield(L, -2, "image");

  lua_newtable(L);
  luaL_setfuncs(L, l_sprite_lib, 0);
  lua_setfield(L, -2, "sprite");

  lua_newtable(L);
  luaL_setfuncs(L, l_spritesheet_lib, 0);
  lua_setfield(L, -2, "spritesheet");

  lua_newtable(L);
  luaL_setfuncs(L, l_graphics_cache_lib, 0);
  lua_setfield(L, -2, "cache");

  lua_newtable(L);
  luaL_setfuncs(L, l_animation_loop_lib, 0);
  lua_setfield(L, -2, "loop");

  lua_newtable(L);
  luaL_setfuncs(L, l_animator_lib, 0);
  lua_setfield(L, -2, "animator");

  lua_newtable(L);
  luaL_setfuncs(L, l_animation_blinker_lib, 0);
  lua_setfield(L, -2, "blinker");

  lua_setfield(L, -2, "animation");

  lua_setfield(L, -2, "graphics");
}
