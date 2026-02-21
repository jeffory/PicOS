#include "lua_bridge_internal.h"
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
// Drives the WiFi state machine and checks for the system menu button.
static void menu_lua_hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  wifi_poll();
  http_lua_fire_pending(L); // fire any queued HTTP Lua callbacks
  if (kbd_consume_menu_press())
    system_menu_show(L);
  // Both screenshot triggers set s_screenshot_pending so the capture fires
  // inside l_display_flush — always on a fully-drawn, flushed frame.
  if (kbd_consume_screenshot_press())
    s_screenshot_pending = true;
  if (screenshot_check_scheduled())
    s_screenshot_pending = true;
}


void lua_bridge_register(lua_State *L) {











  // Open standard Lua libs (but not io/os/package for sandboxing)
  luaL_requiref(L, "_G", luaopen_base, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "table", luaopen_table, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "string", luaopen_string, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "math", luaopen_math, 1);
  lua_pop(L, 1);

  // Create the top-level `picocalc` table
  lua_newtable(L);
  lua_bridge_display_init(L);
  lua_bridge_input_init(L);
  lua_bridge_sys_init(L);
  lua_bridge_fs_init(L);
  lua_bridge_network_init(L);
  lua_bridge_config_init(L);
  lua_bridge_perf_init(L);
  lua_bridge_graphics_init(L);
  lua_bridge_ui_init(L);
  // Set as global
  lua_setglobal(L, "picocalc");

  // Install instruction-count hook for menu button interception.
  // Fires every 256 Lua opcodes (~100µs-1ms) to catch menu button presses
  // even during tight loops, without requiring apps to poll input.
  lua_sethook(L, menu_lua_hook, LUA_MASKCOUNT, 256);
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

  // Drain any keys already held when the error occurred
  do {
    kbd_poll();
    sleep_ms(16);
  } while (kbd_get_buttons());

  // Wait specifically for Esc before returning
  while (true) {
    kbd_poll();
    uint32_t btns = kbd_get_buttons();
    if (btns & BTN_ESC)
      break;
    sleep_ms(16);
  }
  lua_pop(L, 1);
}
