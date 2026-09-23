// Web (Emscripten) platform glue.
//
// The desktop simulator runs Core 1 on a pthread and lets the OS block freely
// (launcher_run() never returns; apps spin in their own loops). A browser tab
// has one thread and freezes unless code returns to the event loop, so here:
//
//   * Core 1's 5 ms service loop becomes web_core1_tick(), run cooperatively.
//   * Every place the OS already waits (hal_sleep_*, display present, the Lua
//     watchdog hook, kbd_poll) calls web_yield*(), which uses ASYNCIFY's
//     emscripten_sleep() to unwind to the browser and resume afterwards.
//
// No core OS file needs to know about any of this.

#include <emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "web_platform.h"

extern void hal_audio_update(void);
extern void mod_player_update(void);
extern void wifi_poll(void);

// Longest the OS may run without handing the tab back to the browser. Longer
// than a frame so apps that flush every frame are paced by present, not this.
#define WEB_MAX_BUSY_MS 20
#define WEB_CORE1_PERIOD_MS 5

static double s_last_yield_ms;
static double s_last_core1_ms;
static bool s_in_core1;

void web_core1_tick(void) {
    double now = emscripten_get_now();
    if (s_in_core1 || now - s_last_core1_ms < WEB_CORE1_PERIOD_MS) return;
    s_in_core1 = true;
    s_last_core1_ms = now;
    hal_audio_update();
    mod_player_update();
    wifi_poll();
    s_in_core1 = false;
}

void web_yield(uint32_t ms) {
    web_core1_tick();
    emscripten_sleep(ms);
    s_last_yield_ms = emscripten_get_now();
    web_core1_tick();
}

void web_yield_if_due(void) {
    web_core1_tick();
    if (emscripten_get_now() - s_last_yield_ms >= WEB_MAX_BUSY_MS) web_yield(0);
}

// ── sim_socket_handler.c hooks (no RPC socket in the browser) ────────────────

bool sim_handler_check_launch(void) { return false; }
void sim_log_append(const char *line) { (void)line; }

static void *s_active_terminal;
void sim_set_active_terminal(void *term) { s_active_terminal = term; }
void *sim_get_active_terminal(void) { return s_active_terminal; }

// Native ELF apps need Unicorn (a JIT), which cannot run inside WASM.
bool unicorn_run_app(const char *elf_path, const char *app_dir,
                     const char *app_id, const char *app_name) {
    (void)elf_path; (void)app_dir; (void)app_id;
    printf("[WEB] Native app '%s' is not supported in the web demo\n", app_name);
    return false;
}
