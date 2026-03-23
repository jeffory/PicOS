#include "lua_psram_alloc.h"
#include "umm_malloc.h"
#include "umm_malloc_cfg.h"
#include "pico/critical_section.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// Hardware spinlock protecting all umm_malloc heap operations across both cores.
// Referenced by UMM_CRITICAL_ENTRY/EXIT in umm_malloc_cfgport.h.
critical_section_t g_umm_critsec;

// Allocate 6 MB of PSRAM for the Lua VM heap
#ifdef PICOS_SIMULATOR
static uint8_t *s_lua_psram_heap = NULL;
uint32_t UMM_MALLOC_CFG_HEAP_SIZE = 8 * 1024 * 1024;
#elif defined(PICO_RP2350)
static uint8_t *s_lua_psram_heap = (uint8_t *)0x11200000;
uint32_t UMM_MALLOC_CFG_HEAP_SIZE = 6 * 1024 * 1024;
#else
static uint8_t s_lua_psram_heap[256 * 1024]; // Fallback to 256K on regular Pico
uint32_t UMM_MALLOC_CFG_HEAP_SIZE = sizeof(s_lua_psram_heap);
#endif

// Satisfy umm_malloc.c externs even if we explicitly use umm_init_heap
#ifdef PICO_RP2350
void *UMM_MALLOC_CFG_HEAP_ADDR = NULL; // We initialize umm_malloc manually
#else
void *UMM_MALLOC_CFG_HEAP_ADDR = NULL; // Initialized in lua_psram_alloc_init
#endif

static int l_panic(lua_State *L) {
  const char *msg = (lua_type(L, -1) == LUA_TSTRING)
                        ? lua_tostring(L, -1)
                        : "error object is not a string";
  printf("PANIC: unprotected error in call to Lua API (%s)\n", msg);
  return 0; /* return to Lua to abort */
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
    printf("[PSRAM] OOM: failed to allocate %zu bytes, %zu free\n",
           nsize, umm_free_heap_size());
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

lua_State *lua_psram_newstate(void) {
  lua_State *L = lua_newstate(lua_psram_alloc, NULL);
  if (L) {
    lua_atpanic(L, &l_panic);
    lua_setwarnf(L, l_warnfoff, L);
  }
  return L;
}
