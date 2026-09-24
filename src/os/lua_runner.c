#include "lua_runner.h"
#include "launcher_types.h"
#include "lua_bridge.h"
#include "lua_psram_alloc.h"
#include "crashlog.h"
#include "config.h"
#include "system_menu.h"
#include "../drivers/audio.h"
#include "../drivers/display.h"
#include "../drivers/fileplayer.h"
#include "../drivers/mp3_player.h"
#include "../drivers/sdcard.h"
#include "../drivers/sound.h"
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

// Launch-time failure before the VM exists: show it, log it, pause so it can
// be read, then hand control back to the launcher.
static void lua_show_launch_failure(const app_entry_t *app, const char *line1,
                                    const char *line2) {
  char heap[96];
  crashlog_describe_heap(heap, sizeof(heap));
  crashlog_write("LUA ERROR", app->name, line1, line2);

  display_clear(C_BG);
  display_draw_text(8, 8, line1, COLOR_RED, C_BG);
  if (line2) display_draw_text(8, 20, line2, COLOR_WHITE, C_BG);
  display_draw_text(8, 36, heap, COLOR_GRAY, C_BG);
  display_flush();
  for (int i = 0; i < 30; i++) {
    watchdog_update();
    sleep_ms(100);
  }
}

static bool lua_run(const app_entry_t *app) {
  printf("[LUA] Starting app '%s', PSRAM free: %zu\n",
         app->name, lua_psram_alloc_free_size());

  // ── Load main.lua from SD ─────────────────────────────────────────────────
  char main_path[160];
  snprintf(main_path, sizeof(main_path), "%s/main.lua", app->path);

  int lua_len = 0;
  char *lua_src = sdcard_read_file(main_path, &lua_len);
  if (!lua_src) {
    // sdcard_read_file returns NULL for both a missing file and a failed
    // PSRAM allocation for its contents; the heap line tells them apart.
    lua_show_launch_failure(app, "Failed to load app:", main_path);
    return false;
  }

  printf("[LUA] Loaded %d bytes, PSRAM free: %zu\n",
         lua_len, lua_psram_alloc_free_size());

  // ── Create Lua VM ─────────────────────────────────────────────────────────
  lua_State *L = lua_psram_newstate();
  if (!L) {
    umm_free(lua_src);
    lua_show_launch_failure(app, "Failed to start app:",
                            "cannot create Lua state (out of PSRAM)");
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

  // Text only: app bundles must not smuggle in unverified bytecode.
  int load_err = luaL_loadbufferx(L, lua_src, lua_len, app->name, "t");
  umm_free(lua_src);

  if (load_err != LUA_OK) {
    lua_bridge_show_error(L, load_err == LUA_ERRMEM ? "Out of memory loading app:"
                                                    : "Load error:");
    lua_close(L);
    return false;
  }

  int run_err = lua_pcall(L, 0, 0, 0);
  if (run_err != LUA_OK) {
    if (!lua_bridge_is_exit_sentinel(L, -1)) {
      lua_bridge_show_error(L, run_err == LUA_ERRMEM ? "Out of memory:"
                                                     : "Runtime error:");
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
  fileplayer_reset();
  sound_init();
  mp3_player_reset();

  return true;
}

static bool lua_can_handle(const app_entry_t *app) {
  return app->type == APP_TYPE_LUA;
}

const AppRunner g_lua_runner = {"lua", lua_can_handle, lua_run};
