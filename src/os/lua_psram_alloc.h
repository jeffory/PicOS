#pragma once

#include "lua.h"
#include <stdbool.h>

// Initialize the PSRAM allocator. Call once on boot.
void lua_psram_alloc_init(void);

// Allocator function compatible with lua_Alloc
void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize);

// Create a new Lua state using the PSRAM allocator
lua_State *lua_psram_newstate(void);

// Memory stats for the PSRAM Lua heap
size_t lua_psram_alloc_free_size(void);
size_t lua_psram_alloc_total_size(void);

// Returns true when free heap has fallen below PSRAM_LOW_WATERMARK.
// Used by the Lua debug hook to trigger GC before allocations start failing.
#define PSRAM_LOW_WATERMARK (512u * 1024u)
bool lua_psram_alloc_is_low(void);
