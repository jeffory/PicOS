#include "lua_bridge_internal.h"
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
                             flip_x, flip_y);
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
                             img->h, false, false);
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
                                 0, draw_w, draw_h, false, false);
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

  display_draw_image_scaled(x, y, img->w, img->h, img->data, scale, angle);
  return 0;
}

static const luaL_Reg l_graphics_image_methods[] = {
    {"getSize", l_graphics_image_getSize},
    {"copy", l_graphics_image_copy},
    {"draw", l_graphics_image_draw},
    {"drawAnchored", l_graphics_image_drawAnchored},
    {"drawTiled", l_graphics_image_drawTiled},
    {"drawScaled", l_graphics_image_drawScaled},
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

static const luaL_Reg l_graphics_lib[] = {
    {"setColor", l_graphics_setColor},
    {"setBackgroundColor", l_graphics_setBackgroundColor},
    {"clear", l_graphics_clear},
    {"drawGrid", l_graphics_drawGrid},
    {"fillBorderedRect", l_graphics_fillBorderedRect},
    {"updateDrawParticles", l_graphics_updateDrawParticles},
    {"draw3DWireframe", l_graphics_draw3DWireframe},
    {NULL, NULL}};


void lua_bridge_graphics_init(lua_State *L) {
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

  // Build picocalc.graphics table
  lua_newtable(L);
  luaL_setfuncs(L, l_graphics_lib, 0);

  lua_newtable(L);
  luaL_setfuncs(L, l_graphics_image_lib, 0);
  lua_setfield(L, -2, "image");

  lua_newtable(L);
  luaL_setfuncs(L, l_graphics_cache_lib, 0);
  lua_setfield(L, -2, "cache");

  lua_setfield(L, -2, "graphics");
}
