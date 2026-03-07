#include "lua_bridge_internal.h"
#include "lua_psram_alloc.h"
#include "../drivers/display.h"
#include "../drivers/http.h"
#include "../drivers/keyboard.h"
#include "../drivers/wifi.h"
#include "../os/clock.h"
#include "../os/config.h"
#include "../os/os.h"
#include "../os/screenshot.h"
#include "../os/system_menu.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "../os/ui.h"

#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_decoders.h"
#include "umm_malloc.h"
#include "umm_malloc_cfg.h"


// ── Colour helpers
// ──────────────────────────────────────────────────────────── Lua passes
// colours as RGB565 integers (or we provide helper constructors)

uint16_t l_checkcolor(lua_State *L, int idx) {
  return (uint16_t)luaL_checkinteger(L, idx);
}

// ── Registration
// ──────────────────────────────────────────────────────────────

// Create a sub-table from a luaL_Reg and attach it to the `picocalc` table
// that is already on the stack at index -1.
void register_subtable(lua_State *L, const char *name,
                              const luaL_Reg *funcs) {
  lua_newtable(L);
  luaL_setfuncs(L, funcs, 0);
  lua_setfield(L, -2, name);
}

// Instruction-count hook: fires every 256 Lua opcodes.
// WiFi is now driven by Core 1 (wifi_poll every 5 ms) — no call needed here.
static void menu_lua_hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  http_lua_fire_pending(L); // fire any queued HTTP Lua callbacks
  if (kbd_consume_menu_press())
    system_menu_show(L);
  // Both screenshot triggers set s_screenshot_pending so the capture fires
  // inside l_display_flush — always on a fully-drawn, flushed frame.
  if (kbd_consume_screenshot_press())
    s_screenshot_pending = true;
  if (screenshot_check_scheduled())
    s_screenshot_pending = true;

  // Low-memory GC trigger: when the PSRAM heap drops below PSRAM_LOW_WATERMARK,
  // force a full GC cycle to reclaim dead Lua objects before allocations start
  // failing.  s_gc_triggered prevents hammering GC every 256 opcodes while
  // memory stays low; it resets once the heap recovers above the watermark.
  static bool s_gc_triggered = false;
  if (lua_psram_alloc_is_low()) {
    if (!s_gc_triggered) {
      printf("[LUA] Memory low (%zu KB free), triggering emergency GC\n",
             lua_psram_alloc_free_size() / 1024);
      lua_gc(L, LUA_GCCOLLECT, 0);
      s_gc_triggered = true;
      printf("[LUA] After GC: %zu KB free\n",
             lua_psram_alloc_free_size() / 1024);
    }
  } else {
    s_gc_triggered = false;
  }
}


void lua_bridge_register(lua_State *L) {
  printf("[LUA] lua_bridge_register start, PSRAM free=%lu\n",
         (unsigned long)umm_free_heap_size());








  // Open standard Lua libs (but not io/os/package for sandboxing)
  printf("[LUA] registering _G...\n");
  luaL_requiref(L, "_G", luaopen_base, 1);
  lua_pop(L, 1);
  printf("[LUA] registering table...\n");
  luaL_requiref(L, "table", luaopen_table, 1);
  lua_pop(L, 1);
  printf("[LUA] registering string...\n");
  luaL_requiref(L, "string", luaopen_string, 1);
  lua_pop(L, 1);
  printf("[LUA] registering math...\n");
  luaL_requiref(L, "math", luaopen_math, 1);
  lua_pop(L, 1);
  printf("[LUA] stdlib done, PSRAM free=%lu\n",
         (unsigned long)umm_free_heap_size());

  // Create the top-level `picocalc` table
  lua_newtable(L);
  printf("[LUA] registering display...\n");
  lua_bridge_display_init(L);
  printf("[LUA] registering input...\n");
  lua_bridge_input_init(L);
  printf("[LUA] registering sys...\n");
  lua_bridge_sys_init(L);
  printf("[LUA] registering fs...\n");
  lua_bridge_fs_init(L);
  printf("[LUA] registering network...\n");
  lua_bridge_network_init(L);
  printf("[LUA] registering config...\n");
  lua_bridge_config_init(L);
  printf("[LUA] registering perf...\n");
  lua_bridge_perf_init(L);
  printf("[LUA] registering graphics...\n");
  lua_bridge_graphics_init(L);
  printf("[LUA] registering ui...\n");
  lua_bridge_ui_init(L);
  printf("[LUA] registering audio...\n");
  lua_bridge_audio_init(L);
  printf("[LUA] registering sound...\n");
  lua_bridge_sound_init(L);
  printf("[LUA] registering repl...\n");
  lua_bridge_repl_init(L);
  printf("[LUA] registering video...\n");
  lua_bridge_video_init(L);
  printf("[LUA] all modules done, PSRAM free=%lu\n",
         (unsigned long)umm_free_heap_size());
  // Set as global
  lua_setglobal(L, "picocalc");

  // Install instruction-count hook for menu button interception.
  // Fires every 256 Lua opcodes (~100µs-1ms) to catch menu button presses
  // even during tight loops, without requiring apps to poll input.
  lua_sethook(L, menu_lua_hook, LUA_MASKCOUNT, 256);
  printf("[LUA] lua_bridge_register complete\n");
}

void lua_bridge_show_error(lua_State *L, const char *context) {
  const char *err = lua_tostring(L, -1);
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", err ? err : "unknown error");

  display_clear(COLOR_BLACK);
  display_draw_text(4, 4, context, COLOR_RED, COLOR_BLACK);

  // Word-wrap the error message at ~52 chars (320px / 6px per char)
  int col = 0, row = 1;
  char line[54] = {0};
  for (int i = 0; buf[i] && row < 38; i++) {
    line[col++] = buf[i];
    if (col >= 52 || buf[i] == '\n') {
      line[col] = '\0';
      display_draw_text(4, 4 + row * 9, line, COLOR_WHITE, COLOR_BLACK);
      row++;
      col = 0;
      memset(line, 0, sizeof(line));
    }
  }
  if (col > 0) {
    line[col] = '\0';
    display_draw_text(4, 4 + row * 9, line, COLOR_WHITE, COLOR_BLACK);
  }

  display_draw_text(4, FB_HEIGHT - 12, "Press Esc to continue", COLOR_GRAY,
                    COLOR_BLACK);
  display_flush();

  // Drain any keys already held when the error occurred.
  // Timeout after ~2s in case the keyboard I2C is dead and state is stale.
  for (int drain = 0; drain < 125 && kbd_get_buttons(); drain++) {
    kbd_poll();
    sleep_ms(16);
  }
  kbd_clear_state();

  // Wait specifically for Esc before returning.
  // Timeout after ~30s so a dead keyboard doesn't block the launcher forever.
  for (int wait = 0; wait < 1875; wait++) {
    kbd_poll();
    if (kbd_get_buttons() & BTN_ESC)
      break;
    sleep_ms(16);
  }
  lua_pop(L, 1);
}
