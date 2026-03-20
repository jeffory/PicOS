// hardware/xip_cache.h stub
#ifndef HARDWARE_XIP_CACHE_H
#define HARDWARE_XIP_CACHE_H

#include <stdint.h>
#include <stddef.h>

// XIP cache operations (no-op on simulator)
static inline void xip_cache_invalidate_all(void) {}
static inline void xip_cache_invalidate_range(uintptr_t start, size_t length) {
    (void)start;
    (void)length;
}
static inline void xip_cache_clean_all(void) {}
static inline void xip_cache_clean_range(uintptr_t start, size_t length) {
    (void)start;
    (void)length;
}
static inline void xip_cache_flush_all(void) {}
static inline void xip_cache_flush_range(uintptr_t start, size_t length) {
    (void)start;
    (void)length;
}

// Cache enable/disable (always "enabled" on simulator)
static inline void xip_cache_enable(void) {}
static inline void xip_cache_disable(void) {}
static inline int xip_cache_is_enabled(void) { return 1; }

#endif // HARDWARE_XIP_CACHE_H
