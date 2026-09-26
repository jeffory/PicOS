#pragma once

// Shared by the PIO PSRAM drivers (pio_psram_qpi.c, pio_psram_bulk.c):
// DMA transfers to and from buffers in the XIP-cached QMI PSRAM window, and
// the transfer lock.
//
// XIP cache rule. A buffer in QMI PSRAM is normally addressed through the
// cached alias (0x11xxxxxx); the DMA must use the uncached alias (+0x04000000)
// and the cache must agree with what it did:
//  - DMA reads from a cached source: clean the source's lines first, so the
//    DMA sees the CPU's latest writes.
//  - DMA writes to a cached destination: the destination's lines must be
//    invalidated before the transfer (a dirty line evicted mid-DMA would
//    write stale bytes over the new data) and after it (a line refetched
//    during the DMA holds the old bytes: the stale read). Only whole lines
//    may be invalidated - an 8-byte line at either end that the buffer
//    shares with its neighbours may hold their pending writes - so the
//    partial lines at the ends are read into a small bounce buffer and
//    copied in by the CPU instead (pio_psram_xip_split).
// The old helpers cleaned the whole cache before a read and never
// invalidated anything after it.
//
// Lock rule. Transfers hold the driver lock one segment
// (PIO_PSRAM_LOCK_SEGMENT bytes) at a time and hand it to a core that is
// waiting between segments (pio_psram_lock_yield), so a 1 MB app transfer on
// Core 0 no longer starves Core 1's MP3 staging refill for its whole length.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PIO_PSRAM_XIP_LINE       8u           // RP2350 XIP_CACHE_LINE_SIZE
#define PIO_PSRAM_XIP_BASE       0x10000000u  // XIP_BASE: cache offsets start here
#define PIO_PSRAM_XIP_UNCACHED   0x04000000u  // cached alias + this = uncached alias
#define PIO_PSRAM_LOCK_SEGMENT   4096u        // bytes per lock hold

static inline bool pio_psram_xip_is_cached(uintptr_t a) {
    return (a >> 24) == 0x11;  // QMI PSRAM (CS1) through the XIP cache
}

// How a cached destination [dst, dst + len) is split: `head` bytes up to the
// first line boundary, `body` whole lines, `tail` bytes after the last one.
// head and tail are each < one line; head + body + tail == len.
typedef struct {
    uint32_t head, body, tail;
} pio_psram_xip_split_t;

static inline pio_psram_xip_split_t pio_psram_xip_split(uintptr_t dst, uint32_t len) {
    pio_psram_xip_split_t s;
    s.head = (uint32_t)((PIO_PSRAM_XIP_LINE - (dst & (PIO_PSRAM_XIP_LINE - 1u))) &
                        (PIO_PSRAM_XIP_LINE - 1u));
    if (s.head > len)
        s.head = len;
    s.body = (len - s.head) & ~(PIO_PSRAM_XIP_LINE - 1u);
    s.tail = len - s.head - s.body;
    return s;
}

#ifndef PICODECK_HOST_TEST

#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/mutex.h"

// ── Transfer lock: a mutex that hands over between segments ────────────────

typedef struct {
    mutex_t mutex;
    volatile uint32_t waiters;  // cores blocked in pio_psram_lock()
} pio_psram_lock_t;

static inline void pio_psram_lock_init(pio_psram_lock_t *l) {
    mutex_init(&l->mutex);
    l->waiters = 0;
}

static inline void pio_psram_lock(pio_psram_lock_t *l) {
    if (mutex_try_enter(&l->mutex, NULL))
        return;
    __atomic_add_fetch(&l->waiters, 1, __ATOMIC_SEQ_CST);
    mutex_enter_blocking(&l->mutex);
    __atomic_sub_fetch(&l->waiters, 1, __ATOMIC_SEQ_CST);
}

static inline void pio_psram_unlock(pio_psram_lock_t *l) {
    mutex_exit(&l->mutex);
}

// Between two segments of one transfer: if the other core is waiting, let
// it in first (a plain unlock + lock would usually win the race back).
static inline void pio_psram_lock_yield(pio_psram_lock_t *l) {
    if (!__atomic_load_n(&l->waiters, __ATOMIC_SEQ_CST))
        return;
    mutex_exit(&l->mutex);
    while (__atomic_load_n(&l->waiters, __ATOMIC_SEQ_CST))
        tight_loop_contents();
    pio_psram_lock(l);
}

// ── XIP cache maintenance around the DMA ────────────────────────────────────

// A DMA-readable view of src: cleans its lines when it is cached.
static inline const uint8_t *pio_psram_xip_src(const uint8_t *src, uint32_t len) {
    uintptr_t a = (uintptr_t)src;
    if (!pio_psram_xip_is_cached(a))
        return src;
    uintptr_t first = a & ~(uintptr_t)(PIO_PSRAM_XIP_LINE - 1u);
    uintptr_t last = (a + len + PIO_PSRAM_XIP_LINE - 1u) & ~(uintptr_t)(PIO_PSRAM_XIP_LINE - 1u);
    xip_cache_clean_range(first - PIO_PSRAM_XIP_BASE, last - first);
    return (const uint8_t *)(a + PIO_PSRAM_XIP_UNCACHED);
}

// A raw read into DMA-safe memory (SRAM or an uncached alias).
typedef void (*pio_psram_raw_read_fn)(uint32_t addr, uint8_t *dst, uint32_t len);

// Read len bytes at addr into dst, coherently with the XIP cache (see the
// rule at the top of this file).
static inline void pio_psram_xip_read(uint32_t addr, uint8_t *dst, uint32_t len,
                                      pio_psram_raw_read_fn raw) {
    if (!pio_psram_xip_is_cached((uintptr_t)dst)) {
        raw(addr, dst, len);
        return;
    }
    pio_psram_xip_split_t s = pio_psram_xip_split((uintptr_t)dst, len);
    uint8_t edge[PIO_PSRAM_XIP_LINE];
    if (s.head) {
        raw(addr, edge, s.head);
        memcpy(dst, edge, s.head);  // CPU store through the cache
    }
    if (s.body) {
        uint8_t *body = dst + s.head;
        uintptr_t off = (uintptr_t)body - PIO_PSRAM_XIP_BASE;
        xip_cache_invalidate_range(off, s.body);
        raw(addr + s.head, (uint8_t *)((uintptr_t)body + PIO_PSRAM_XIP_UNCACHED), s.body);
        xip_cache_invalidate_range(off, s.body);
    }
    if (s.tail) {
        raw(addr + s.head + s.body, edge, s.tail);
        memcpy(dst + s.head + s.body, edge, s.tail);
    }
}

#endif  // !PICODECK_HOST_TEST
