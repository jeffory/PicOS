#include "lua_psram_alloc.h"
#include "umm_malloc.h"
#include "umm_malloc_cfg.h"
#include <stdint.h>
#include <stdio.h>

// Allocate 6 MB of PSRAM for the Lua VM heap
#ifdef PICO_RP2350
static uint8_t *s_lua_psram_heap = (uint8_t *)0x11200000;
#else
static uint8_t s_lua_psram_heap[256 * 1024]; // Fallback to 256K on regular Pico
#endif

#ifdef PICO_RP2350
// Satisfy umm_malloc.c externs even if we explicitly use umm_init_heap
void *UMM_MALLOC_CFG_HEAP_ADDR = NULL; // We initialize umm_malloc manually
uint32_t UMM_MALLOC_CFG_HEAP_SIZE = 6 * 1024 * 1024;
#else
// Satisfy umm_malloc.c externs even if we explicitly use umm_init_heap
void *UMM_MALLOC_CFG_HEAP_ADDR = s_lua_psram_heap;
uint32_t UMM_MALLOC_CFG_HEAP_SIZE = sizeof(s_lua_psram_heap);
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
  } else {
    return umm_realloc(ptr, nsize);
  }
}

size_t lua_psram_alloc_free_size(void) {
  return umm_free_heap_size();
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
