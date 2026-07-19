// hardware/sync.h stub for simulator
// Provides no-op spinlock primitives used by http.h/http.c

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef int spin_lock_t;

static inline int spin_lock_claim_unused(bool required) {
    (void)required;
    return 0;
}

static inline spin_lock_t *spin_lock_instance(int num) {
    (void)num;
    static spin_lock_t dummy;
    return &dummy;
}

static inline uint32_t spin_lock_blocking(spin_lock_t *lock) {
    (void)lock;
    return 0;
}

static inline void spin_unlock(spin_lock_t *lock, uint32_t saved_irq) {
    (void)lock;
    (void)saved_irq;
}
