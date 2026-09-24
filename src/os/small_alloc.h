// Small-object pools in front of umm for the Lua heap.
//
// umm_malloc hands out whole 200-byte blocks (UMM_BLOCK_BODY_SIZE, forced by
// its 15-bit block indices on a 6 MB heap), and almost every Lua object is
// far smaller: a table is 32 bytes on the device, a short string 17-40, a
// closure 20-32. At one block each, the heap holds at most ~30,800 live
// objects however small they are. This module carves size-class slabs out of
// the backing heap and packs small requests into them.
//
// Contract:
//   - It is the Lua allocator's policy, not a general malloc: requests of up
//     to SMALL_ALLOC_MAX bytes are pooled, larger ones (and small ones when no
//     slab can be had) go straight to the backing allocator. Every block it
//     returns - pooled or not - must come back through small_free /
//     small_realloc on the same heap; ownership is decided by address, never
//     by the caller's size.
//   - Slabs come from, and go back to, the backing allocator (umm on the
//     device), so umm's free / largest-block / fragmentation figures keep
//     describing the real heap. An empty slab is returned at once, except one
//     spare kept to stop alloc/free ping-pong; small_destroy returns the rest.
//   - Not thread-safe: one heap is used from one core (the Lua VM, Core 0).
//     The backing allocator provides its own locking (umm's critical section).
//   - Uses no static memory: the heap and its slab directory live in memory
//     from the backing allocator.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Size classes are defined for the 32-bit device. On a 64-bit host (the
// simulator, unit tests) every Lua struct is about twice as big, so classes,
// alignment and slabs scale with the pointer size to pool the same objects.
#define SMALL_ALLOC_SCALE   (sizeof(void *) / 4)
#define SMALL_ALLOC_ALIGN   (8 * SMALL_ALLOC_SCALE)
#define SMALL_ALLOC_MAX     (128 * SMALL_ALLOC_SCALE)
#define SMALL_CLASS_COUNT   11

// One umm block is 200 bytes and a umm allocation of n blocks holds
// n * 200 - 4 bytes, so a slab of 21 blocks (4196 bytes; 42 blocks on a
// 64-bit host) wastes nothing in umm. lua_psram_alloc.c asserts the block
// size against umm_malloc_cfgport.h.
#define SMALL_UMM_BLOCK_BYTES 200u
#define SMALL_SLAB_BYTES \
  ((size_t)(21u * SMALL_ALLOC_SCALE) * SMALL_UMM_BLOCK_BYTES - 4u)

typedef struct {
  void *(*alloc)(size_t size);
  void *(*realloc)(void *ptr, size_t size);
  void  (*free)(void *ptr);
} small_backing_t;

typedef struct small_heap small_heap_t;

typedef struct {
  uint32_t slabs;         // slabs held (including the spare)
  uint32_t slab_bytes;    // backing bytes held by slabs (slabs * SMALL_SLAB_BYTES)
  uint32_t objects;       // live pooled objects
  uint32_t object_bytes;  // bytes of their size classes
} small_stats_t;

// Heap bookkeeping comes from `backing` (copied). NULL when out of memory.
small_heap_t *small_create(const small_backing_t *backing);
// Returns every slab and the heap itself to the backing allocator. Pooled
// objects still live become invalid; blocks passed through to the backing
// allocator are untouched (they belong to it).
void small_destroy(small_heap_t *h);

// The three entry points. `osize` is the caller's (Lua's) old size of `ptr`,
// used only to bound copies; nsize == 0 frees. They never mix pools: a
// pooled pointer is released to its slab, any other to the backing allocator.
void *small_alloc(small_heap_t *h, size_t size);
void  small_free(small_heap_t *h, void *ptr, size_t osize);
void *small_realloc(small_heap_t *h, void *ptr, size_t osize, size_t nsize);

// True if `ptr` lies inside one of this heap's slabs.
bool small_owns(const small_heap_t *h, const void *ptr);

void small_stats(const small_heap_t *h, small_stats_t *out);

// Size of class `cls` (0..SMALL_CLASS_COUNT-1); the class a request of `size`
// bytes (1..SMALL_ALLOC_MAX) lands in. For tests and stats.
size_t small_class_size(int cls);
int    small_class_of(size_t size);
