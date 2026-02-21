#include "lua_bridge_internal.h"

// ── picocalc.perf.* ──────────────────────────────────────────────────────────
// Performance monitoring utilities for apps

#define PERF_SAMPLES 30

static uint32_t s_perf_frame_times[PERF_SAMPLES] = {0};
static int s_perf_index = 0;
static uint32_t s_perf_frame_start = 0;
static uint32_t s_perf_last_frame_time = 0;
static int s_perf_fps = 0;

// Start timing a frame. Call at the beginning of your game loop.
static int l_perf_beginFrame(lua_State *L) {
  (void)L;
  // Initialize start time on the very first frame to avoid a huge initial
  // delta, but don't overwrite it on subsequent frames. This ensures that the
  // total frame loop time (including sys.sleep after endFrame) is captured.
  if (s_perf_frame_start == 0) {
    s_perf_frame_start = to_ms_since_boot(get_absolute_time());
  }
  return 0;
}

// End timing a frame and calculate FPS. Call at the end of your game loop.
static int l_perf_endFrame(lua_State *L) {
  (void)L;
  uint32_t now = to_ms_since_boot(get_absolute_time());

  if (s_perf_frame_start != 0) {
    uint32_t delta = now - s_perf_frame_start;

    s_perf_last_frame_time = delta;
    s_perf_frame_times[s_perf_index] = delta;
    s_perf_index = (s_perf_index + 1) % PERF_SAMPLES;

    // Calculate average frame time
    uint32_t sum = 0;
    int count = 0;
    for (int i = 0; i < PERF_SAMPLES; i++) {
      if (s_perf_frame_times[i] > 0) {
        sum += s_perf_frame_times[i];
        count++;
      }
    }
    uint32_t avg_frame_time = (count > 0) ? (sum / count) : 0;

    // Calculate FPS (avoid divide by zero)
    s_perf_fps = (avg_frame_time > 0) ? (1000 / avg_frame_time) : 0;
  }

  // Anchor the start of the next measurement to *now*, capturing any
  // sys.sleep() block or loop overhead that occurs outside of begin/end.
  s_perf_frame_start = now;

  return 0;
}

// Get current FPS (averaged over recent frames)
static int l_perf_getFPS(lua_State *L) {
  lua_pushinteger(L, s_perf_fps);
  return 1;
}

// Get last frame time in milliseconds
static int l_perf_getFrameTime(lua_State *L) {
  lua_pushinteger(L, s_perf_last_frame_time);
  return 1;
}

// Convenience: draw FPS counter at specified position with color coding
static int l_perf_drawFPS(lua_State *L) {
  int x = (int)luaL_optinteger(L, 1, 250); // default top-right
  int y = (int)luaL_optinteger(L, 2, 8);

  char buf[16];
  snprintf(buf, sizeof(buf), "FPS: %d", s_perf_fps);

  // Color code: green >= 55, yellow >= 30, red < 30
  uint16_t color = (s_perf_fps >= 55)   ? COLOR_GREEN
                   : (s_perf_fps >= 30) ? COLOR_YELLOW
                                        : COLOR_RED;

  display_draw_text(x, y, buf, color, COLOR_BLACK);
  return 0;
}

static const luaL_Reg l_perf_lib[] = {
    {"beginFrame", l_perf_beginFrame}, {"endFrame", l_perf_endFrame},
    {"getFPS", l_perf_getFPS},         {"getFrameTime", l_perf_getFrameTime},
    {"drawFPS", l_perf_drawFPS},       {NULL, NULL}};


void lua_bridge_perf_init(lua_State *L) {
  
  s_perf_frame_start = 0;
  s_perf_index = 0;
  s_perf_fps = 0;
  s_perf_last_frame_time = 0;
  memset(s_perf_frame_times, 0, sizeof(s_perf_frame_times));

register_subtable(L, "perf", l_perf_lib);
}
