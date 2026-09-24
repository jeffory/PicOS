#include "lua_runner.h"
#include "launcher_types.h"
#include "lua_bridge.h"
#include "app_stack.h"
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

// ── VM stack ─────────────────────────────────────────────────────────────────
// The whole VM lifetime (state creation, bridge registration, parse, run,
// error screen, lua_close and its __gc handlers) runs on its own PSP stack
// (app_stack.h), not on Core 0's 4 KB main stack. Everything the VM calls
// runs there too: the debug hook, the system menu, HTTP callbacks,
// sys.sleep, modal dialogs and the dev-command pump.
//
// Sizing: LUAI_MAXCCALLS (cmake/picos_lua.cmake) caps nested C calls /
// parser levels at 60, so the stack must hold 60 of the deepest C level in
// the VM: a string.gsub callback (str_gsub 640 B + lua_callk, ccall,
// luaD_precall, precallC, luaV_execute; measured on device at 864 B per
// level), plus the frames below the VM and room for a leaf at the deepest
// level (pattern matching recursion, error formatting). Measured peak at
// the ~58-level "C stack overflow" error: 51,428 B, so a limit hit
// surfaces as that Lua error, not a HardFault; 32 KB would fault at ~36
// levels. Typical apps peak at 2-6 KB (launch, HTTPS, zip.extract + PNG).
//
// PSRAM, not SRAM: the SRAM heap is ~2.6 KB after the framebuffers and
// static buffers (link map), so no SRAM region of this size exists.
// PSRAM-stack-sensitive path: picocalc.video play/stop boosts sysclk to
// 300 MHz via launcher_apply_clock(), which retimes QMI M1 (this stack's
// memory) while running on this stack. Verified on hardware (5 play/stop
// cycles x 11 runs, no fault); keep flash writes and QMI direct-mode code
// off this stack entirely.
#define LUA_VM_STACK_SIZE (64u * 1024u)

typedef struct {
  const app_entry_t *app;
  char *lua_src;      // freed by the VM body once parsed
  int lua_len;
  bool ran;           // the app body ran (false: failed before it)
} lua_vm_ctx_t;

// Runs on the Lua VM stack (PSP).
static void lua_vm_body(void *arg) {
  lua_vm_ctx_t *ctx = (lua_vm_ctx_t *)arg;
  const app_entry_t *app = ctx->app;

  // ── Create Lua VM ─────────────────────────────────────────────────────────
  lua_State *L = lua_psram_newstate();
  if (!L) {
    umm_free(ctx->lua_src);
    lua_show_launch_failure(app, "Failed to start app:",
                            "cannot create Lua state (out of PSRAM)");
    return;
  }

  printf("[LUA] Lua state created, PSRAM free: %zu\n", lua_psram_alloc_free_size());

  // Wrap registration in a pcall to catch errors before we enter the main loop.
  // This prevents abort() if a module fails to register (e.g. OOM).
  lua_pushcfunction(L, (lua_CFunction)lua_bridge_register);
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    lua_bridge_show_error(L, "Init error:");
    lua_close(L);
    umm_free(ctx->lua_src);
    return;
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
  int load_err = luaL_loadbufferx(L, ctx->lua_src, ctx->lua_len, app->name, "t");
  umm_free(ctx->lua_src);

  if (load_err != LUA_OK) {
    lua_bridge_show_error(L, load_err == LUA_ERRMEM ? "Out of memory loading app:"
                                                    : "Load error:");
    lua_close(L);
    return;
  }

  ctx->ran = true;
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
}

static bool lua_run(const app_entry_t *app) {
  printf("[LUA] Starting app '%s', PSRAM free: %zu\n",
         app->name, lua_psram_alloc_free_size());

  // ── VM stack (before anything else, so it sits below the VM's blocks) ────
  uint8_t *stack = (uint8_t *)umm_malloc(LUA_VM_STACK_SIZE);
  if (!stack) {
    lua_show_launch_failure(app, "Failed to start app:",
                            "no PSRAM block for the 64 KB Lua stack");
    return false;
  }

  // ── Load main.lua from SD ─────────────────────────────────────────────────
  char main_path[160];
  snprintf(main_path, sizeof(main_path), "%s/main.lua", app->path);

  int lua_len = 0;
  char *lua_src = sdcard_read_file(main_path, &lua_len);
  if (!lua_src) {
    umm_free(stack);
    // sdcard_read_file returns NULL for both a missing file and a failed
    // PSRAM allocation for its contents; the heap line tells them apart.
    lua_show_launch_failure(app, "Failed to load app:", main_path);
    return false;
  }

  printf("[LUA] Loaded %d bytes, PSRAM free: %zu\n",
         lua_len, lua_psram_alloc_free_size());

  lua_vm_ctx_t ctx = {app, lua_src, lua_len, false};
  app_stack_run(stack, LUA_VM_STACK_SIZE, APP_STACK_LUA, lua_vm_body, &ctx);

  uint32_t peak = app_stack_high_water(stack, LUA_VM_STACK_SIZE);
  if (peak)
    printf("[LUA] Stack high-water: %lu of %lu bytes\n", (unsigned long)peak,
           (unsigned long)LUA_VM_STACK_SIZE);
  if (!app_stack_guard_intact(stack)) {
    // PSPLIM should have faulted first; reaching here means something wrote
    // below the limit without pushing (a stray pointer, or a large local
    // array written from its low end).
    crashlog_write("LUA ERROR", app->name, "Lua stack guard overwritten",
                   "64 KB VM stack");
  }
  umm_free(stack);

  if (!ctx.ran)
    return false;

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
