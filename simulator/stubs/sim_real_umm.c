// The firmware's umm_malloc, compiled into the simulator under sim_real_umm_*
// names for the opt-in --real-umm mode (driver_stubs.c dispatches umm_* here
// when it is on).
//
// By default the simulator's umm_* is a counting allocator over the host
// malloc (ASan sees every block; see stubs/umm_malloc.h). That cannot show
// what the device heap really costs: umm hands out whole 200-byte blocks
// (UMM_BLOCK_BODY_SIZE), fragments, and has a largest-free-block limit. With
// --real-umm the simulator runs the same third_party/umm_malloc source with
// the same src/os/umm_malloc_cfgport.h on a device-sized arena, so
// psram_free / psram_largest_block / psram_fragmentation move the way they do
// on hardware (block-size waste included). Pointer sizes still differ: on the
// 64-bit host Lua objects are larger than on the 32-bit device.
//
// Every external symbol of umm_malloc.c is renamed so it cannot collide with
// the counting allocator's umm_* in driver_stubs.c.

#define umm_init_heap                  sim_real_umm_init_heap
#define umm_init                       sim_real_umm_init
#define umm_malloc                     sim_real_umm_malloc
#define umm_calloc                     sim_real_umm_calloc
#define umm_realloc                    sim_real_umm_realloc
#define umm_free                       sim_real_umm_free
#define umm_info                       sim_real_umm_info
#define umm_free_heap_size             sim_real_umm_free_heap_size
#define umm_max_free_block_size        sim_real_umm_max_free_block_size
#define umm_usage_metric               sim_real_umm_usage_metric
#define umm_fragmentation_metric       sim_real_umm_fragmentation_metric
#define umm_multi_init_heap            sim_real_umm_multi_init_heap
#define umm_multi_init                 sim_real_umm_multi_init
#define umm_multi_malloc               sim_real_umm_multi_malloc
#define umm_multi_calloc               sim_real_umm_multi_calloc
#define umm_multi_realloc              sim_real_umm_multi_realloc
#define umm_multi_free                 sim_real_umm_multi_free
#define umm_multi_info                 sim_real_umm_multi_info
#define umm_multi_free_heap_size       sim_real_umm_multi_free_heap_size
#define umm_multi_max_free_block_size  sim_real_umm_multi_max_free_block_size
#define umm_multi_usage_metric         sim_real_umm_multi_usage_metric
#define umm_multi_fragmentation_metric sim_real_umm_multi_fragmentation_metric
#define umm_heap_current               sim_real_umm_heap_current
#define ummHeapInfo                    sim_real_ummHeapInfo
#define compute_usage_metric           sim_real_compute_usage_metric
#define compute_fragmentation_metric   sim_real_compute_fragmentation_metric

// Quote-form includes inside umm_malloc.c resolve next to it, so it gets the
// real umm_malloc.h / umm_malloc_cfg.h, not stubs/umm_malloc.h; the
// <umm_malloc_cfgport.h> it pulls in is the firmware's src/os copy (its
// critical section is a pthread mutex through stubs/pico/critical_section.h).
#include "../../third_party/umm_malloc/src/umm_malloc.c"
