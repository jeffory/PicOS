// umm_malloc.h stub for simulator
//
// umm_* is backed by the host malloc through a counting allocator
// (simulator/stubs/driver_stubs.c): it tracks live and peak bytes with
// malloc_usable_size, so umm_free_heap_size() reports 8 MB minus the live
// umm/Lua allocations and a leak shows up in get_heap_info. Host malloc (not
// a real umm heap) keeps every allocation visible to ASan.
//
// These are real functions, not macros onto malloc: TUs that include the real
// third_party/umm_malloc/src/umm_malloc.h (lua_bridge_internal.h does) and TUs
// that include this stub must reach the same counter.
//
// --real-umm swaps the counting allocator for the firmware's umm_malloc on a
// device-sized arena (stubs/sim_real_umm.c): 200-byte blocks, fragmentation
// and the largest-free-block limit then behave as on hardware, but ASan no
// longer sees inside the heap. For heap measurements, not sanitizer runs.

#ifndef UMM_MALLOC_H
#define UMM_MALLOC_H

#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>

void *umm_malloc(size_t size);
void *umm_calloc(size_t num, size_t size);
void *umm_realloc(void *ptr, size_t size);
void umm_free(void *ptr);

// Simulated heap size of the counting allocator.
#define SIM_UMM_HEAP_SIZE (8u * 1024u * 1024u)
// --real-umm arena: the RP2350 UMM_MALLOC_CFG_HEAP_SIZE (6 MB minus the
// 128 KB Core 1 pool, lua_psram_alloc.c). It must stay under 32767 umm blocks
// of 200 bytes (15-bit block indices), which 8 MB would not.
#define SIM_REAL_UMM_HEAP_SIZE (6u * 1024u * 1024u - 128u * 1024u)

// Switch umm_* to the real umm_malloc (call before the first allocation;
// false if the arena could not be allocated). sim_umm_heap_size() is the
// heap size of whichever allocator is active.
bool   sim_umm_use_real(void);
bool   sim_umm_is_real(void);
size_t sim_umm_heap_size(void);

// Counting mode: 8 MB minus live bytes; the largest block is approximated by
// the free size (the host heap does not fragment the way umm does),
// fragmentation is 0. --real-umm: umm's own figures.
size_t umm_free_heap_size(void);
size_t umm_max_free_block_size(void);
int umm_fragmentation_metric(void);

// Live and peak bytes currently held through umm_*.
size_t sim_umm_live_bytes(void);
size_t sim_umm_peak_bytes(void);

struct umm_heap_info {
    size_t total_size;
    size_t used_size;
    size_t free_size;
    size_t max_used_size;
};

static inline void umm_info_get(struct umm_heap_info* info) {
    if (info) {
        info->total_size = sim_umm_heap_size();
        info->used_size = sim_umm_live_bytes();
        info->free_size = umm_free_heap_size();
        info->max_used_size = sim_umm_peak_bytes();
    }
}

// Initialize/finalize (no-ops: the host heap needs no setup)
static inline void umm_init(void) {}
static inline void umm_fini(void) {}

static inline void umm_init_heap(void* heap, size_t heap_size) {
    (void)heap;
    (void)heap_size;
}

#endif // UMM_MALLOC_H
