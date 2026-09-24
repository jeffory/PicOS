// umm_* on the host heap for unit tests (declared by simulator/stubs/umm_malloc.h).
#include "umm_malloc.h"

void *umm_malloc(size_t size) { return malloc(size); }
void *umm_calloc(size_t num, size_t size) { return calloc(num, size); }
void *umm_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void umm_free(void *ptr) { free(ptr); }
size_t umm_free_heap_size(void) { return SIM_UMM_HEAP_SIZE; }
size_t umm_max_free_block_size(void) { return SIM_UMM_HEAP_SIZE; }
