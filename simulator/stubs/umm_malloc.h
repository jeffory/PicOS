// umm_malloc.h stub for simulator
// Maps umm_malloc to standard malloc

#ifndef UMM_MALLOC_H
#define UMM_MALLOC_H

#include <stdlib.h>
#include <stddef.h>

// umm_malloc is just standard malloc on PC
#define umm_malloc(size) malloc(size)
#define umm_free(ptr) free(ptr)
#define umm_realloc(ptr, size) realloc(ptr, size)
#define umm_calloc(num, size) calloc(num, size)

// Heap info structure (dummy)
struct umm_heap_info {
    size_t total_size;
    size_t used_size;
    size_t free_size;
    size_t max_used_size;
};

static inline void umm_info_get(struct umm_heap_info* info) {
    if (info) {
        info->total_size = 8 * 1024 * 1024;  // 8MB simulated
        info->used_size = 0;
        info->free_size = 8 * 1024 * 1024;
        info->max_used_size = 0;
    }
}

// Initialize/finalize (no-ops)
static inline void umm_init(void) {}
static inline void umm_fini(void) {}

// umm_init_heap - initialize with specific heap (no-op on simulator, just use regular init)
static inline void umm_init_heap(void* heap, size_t heap_size) {
    (void)heap;
    (void)heap_size;
}

#endif // UMM_MALLOC_H
