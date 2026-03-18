#include "lua_runner.h"
#include "launcher_types.h"
#include "lua_bridge.h"
#include "lua_psram_alloc.h"
#include "config.h"
#include "system_menu.h"
#include "../drivers/audio.h"
#include "../drivers/display.h"
#include "../drivers/sdcard.h"
#include "../drivers/wifi.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "umm_malloc.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#define C_BG COLOR_BLACK

static bool lua_run(const app_entry_t *app) {
  printf("[LUA] Starting app '%s', PSRAM free: %zu\n",
         app->name, lua_psram_alloc_free_size());

  // ── Load main.lua from SD ─────────────────────────────────────────────────
  char main_path[160];
  snprintf(main_path, sizeof(main_path), "%s/main.lua", app->path);

  int lua_len = 0;
  char *lua_src = sdcard_read_file(main_path, &lua_len);
  if (!lua_src) {
    display_clear(C_BG);
    display_draw_text(8, 8, "Failed to load app:", COLOR_RED, C_BG);
    display_draw_text(8, 20, main_path, COLOR_WHITE, C_BG);
    display_flush();
    watchdog_update();
    sleep_ms(2000);
    return false;
  }

  printf("[LUA] Loaded %d bytes, PSRAM free: %zu\n",
         lua_len, lua_psram_alloc_free_size());

  // ── Create Lua VM ─────────────────────────────────────────────────────────
  lua_State *L = lua_psram_newstate();
  if (!L) {
    printf("[LUA] FAILED: lua_psram_newstate returned NULL\n");
    umm_free(lua_src);
    return false;
  }

  printf("[LUA] Lua state created, PSRAM free: %zu\n", lua_psram_alloc_free_size());

  // Wrap registration in a pcall to catch errors before we enter the main loop.
  // This prevents abort() if a module fails to register (e.g. OOM).
  lua_pushcfunction(L, (lua_CFunction)lua_bridge_register);
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    lua_bridge_show_error(L, "Init error:");
    lua_close(L);
    umm_free(lua_src);
    return false;
  }

  // ── Set app globals ───────────────────────────────────────────────────────
  lua_pushstring(L, app->path);
  lua_setglobal(L, "APP_DIR");
  lua_pushstring(L, app->name);
  lua_setglobal(L, "APP_NAME");
  lua_pushstring(L, app->id);
  lua_setglobal(L, "APP_ID");

  lua_newtable(L);
  lua_pushboolean(L, app->has_root_filesystem);
  lua_setfield(L, -2, "root_filesystem");
  lua_pushboolean(L, app->has_http);
  lua_setfield(L, -2, "http");
  lua_pushboolean(L, app->has_audio);
  lua_setfield(L, -2, "audio");
  lua_setglobal(L, "APP_REQUIREMENTS");

  // ── Load and execute ──────────────────────────────────────────────────────
  display_clear(C_BG);
  display_flush();

  int load_err = luaL_loadbuffer(L, lua_src, lua_len, app->name);
  umm_free(lua_src);

  if (load_err != LUA_OK) {
    lua_bridge_show_error(L, "Load error:");
    lua_close(L);
    return false;
  }

  int run_err = lua_pcall(L, 0, 0, 0);
  if (run_err != LUA_OK) {
    if (!lua_bridge_is_exit_sentinel(L, -1)) {
      lua_bridge_show_error(L, "Runtime error:");
    } else {
      lua_pop(L, 1); // discard exit sentinel
    }
  }

  lua_close(L);

  // Ensure no audio leaks into the next app or launcher.
  // lua_close() runs __gc handlers which destroy fileplayer/mp3player objects,
  // but audio_play_tone() and audio_start_stream() have no Lua userdata —
  // a leftover tone or stream will keep playing into the next app.
  audio_stop_tone();
  audio_stop_stream();

  return true;
}

static bool lua_can_handle(const app_entry_t *app) {
  return app->type == APP_TYPE_LUA;
}

const AppRunner g_lua_runner = {"lua", lua_can_handle, lua_run};
