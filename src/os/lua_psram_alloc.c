#include "lua_psram_alloc.h"
#include "crashlog.h"
#include "launcher.h"
#include "../drivers/display.h"
#include "umm_malloc.h"
#include "umm_malloc_cfg.h"
#include "hardware/watchdog.h"
#include "pico/critical_section.h"
#include "pico/stdlib.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Hardware spinlock protecting all umm_malloc heap operations across both cores.
// Referenced by UMM_CRITICAL_ENTRY/EXIT in umm_malloc_cfgport.h.
critical_section_t g_umm_critsec;

// Allocate PSRAM for the Lua VM heap.
// On RP2350, reserve 128KB at the end of the 6MB region for Core 1's
// dedicated allocator (WiFi/Mongoose) to eliminate cross-core contention.
#ifdef PICOS_SIMULATOR
  #define CORE1_POOL_SIZE 0
  static uint8_t *s_lua_psram_heap = NULL;
  uint32_t UMM_MALLOC_CFG_HEAP_SIZE = 8 * 1024 * 1024;
#elif defined(PICO_RP2350)
  #define CORE1_POOL_SIZE (128 * 1024)
  static uint8_t *s_lua_psram_heap = (uint8_t *)0x11200000;
  uint32_t UMM_MALLOC_CFG_HEAP_SIZE = 6 * 1024 * 1024 - CORE1_POOL_SIZE;
#else
  #define CORE1_POOL_SIZE 0
  static uint8_t s_lua_psram_heap[256 * 1024]; // Fallback to 256K on regular Pico
  uint32_t UMM_MALLOC_CFG_HEAP_SIZE = sizeof(s_lua_psram_heap);
#endif

// Satisfy umm_malloc.c externs even if we explicitly use umm_init_heap
#ifdef PICO_RP2350
void *UMM_MALLOC_CFG_HEAP_ADDR = NULL; // We initialize umm_malloc manually
#else
void *UMM_MALLOC_CFG_HEAP_ADDR = NULL; // Initialized in lua_psram_alloc_init
#endif

// Unprotected Lua error (an error raised outside any pcall — typically an
// allocation failure inside lua_newstate/lua_close or a bridge bug).  Lua
// would call abort() when this returns, which halts the core until the 10s
// watchdog fires and reboots with no record of what happened.  Instead:
// record the error and heap state to /system/error.log, show it on screen,
// then reboot deliberately.
static int l_panic(lua_State *L) {
  const char *msg = (lua_type(L, -1) == LUA_TSTRING)
                        ? lua_tostring(L, -1)
                        : "error object is not a string";
  const char *app = launcher_get_running_app_name();
  char heap[96];
  crashlog_describe_heap(heap, sizeof(heap));
  printf("PANIC: unprotected error in call to Lua API (%s) [%s]\n", msg, heap);

  crashlog_write("LUA PANIC", app, "unprotected error in call to Lua API", msg);
  // The panic is now on record with its app name; drop the dirty-exit marker
  // so the next boot does not report the same event a second time.
  crashlog_clear_running();

  display_clear(COLOR_BLACK);
  display_draw_text(4, 4, "Lua panic (unprotected error):", COLOR_RED, COLOR_BLACK);
  // Word-wrap the message at 52 columns.
  int col = 0, row = 1;
  char line[54] = {0};
  for (int i = 0; msg[i] && row < 30; i++) {
    if (msg[i] != '\n') line[col++] = msg[i];
    if (col >= 52 || msg[i] == '\n') {
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
    row++;
  }
  row++;
  display_draw_text(4, 4 + row * 9, heap, COLOR_GRAY, COLOR_BLACK);
  if (app) {
    row++;
    display_draw_text(4, 4 + row * 9, app, COLOR_GRAY, COLOR_BLACK);
  }
  display_draw_text(4, FB_HEIGHT - 12, "Logged to /system/error.log - rebooting",
                    COLOR_GRAY, COLOR_BLACK);
  display_flush();

  for (int i = 0; i < 40; i++) {
    watchdog_update();
    sleep_ms(100);
  }
  stdio_flush();
  watchdog_reboot(0, 0, 0);
  for (;;) tight_loop_contents();
  return 0;
}

static void l_warnfoff(void *ud, const char *message, int tocont) {
  (void)ud;
  (void)message;
  (void)tocont;
}

void lua_psram_alloc_init(void) {
  critical_section_init(&g_umm_critsec);
#ifdef PICOS_SIMULATOR
  if (!s_lua_psram_heap) {
    s_lua_psram_heap = malloc(UMM_MALLOC_CFG_HEAP_SIZE);
    if (!s_lua_psram_heap) {
      printf("FATAL: Failed to allocate Lua PSRAM heap in simulator\n");
      return;
    }
  }
#endif
  umm_init_heap(s_lua_psram_heap, UMM_MALLOC_CFG_HEAP_SIZE);
  printf("PSRAM Lua Allocator Initialized: %d bytes\n",
         (int)UMM_MALLOC_CFG_HEAP_SIZE);
  
  // Debug: check free size immediately after init
  size_t free_after_init = umm_free_heap_size();
  printf("[PSRAM] Free immediately after init: %zu bytes (%zuK)\n", 
         free_after_init, free_after_init / 1024);
}

void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;
  (void)osize;

  if (nsize == 0) {
    umm_free(ptr);
    return NULL;
  }

  void *result = umm_realloc(ptr, nsize);
  if (!result) {
    printf("[PSRAM] OOM: failed to allocate %zu bytes (free=%zu largest=%zu frag=%d%%)\n",
           nsize, umm_free_heap_size(), lua_psram_alloc_largest_block(),
           lua_psram_alloc_fragmentation());
  }
  return result;
}

size_t lua_psram_alloc_free_size(void) {
  return umm_free_heap_size();
}

bool lua_psram_alloc_is_low(void) {
  return umm_free_heap_size() < PSRAM_LOW_WATERMARK;
}

size_t lua_psram_alloc_total_size(void) {
  return (size_t)UMM_MALLOC_CFG_HEAP_SIZE;
}

size_t lua_psram_alloc_largest_block(void) {
  return umm_max_free_block_size();
}

int lua_psram_alloc_fragmentation(void) {
  return umm_fragmentation_metric();
}

lua_State *lua_psram_newstate(void) {
  lua_State *L = lua_newstate(lua_psram_alloc, NULL);
  if (L) {
    lua_atpanic(L, &l_panic);
    lua_setwarnf(L, l_warnfoff, L);
  }
  return L;
}

void *lua_psram_get_core1_pool(void) {
#if CORE1_POOL_SIZE > 0
    return s_lua_psram_heap + UMM_MALLOC_CFG_HEAP_SIZE;
#else
    return NULL;
#endif
}

size_t lua_psram_get_core1_pool_size(void) {
    return CORE1_POOL_SIZE;
}
