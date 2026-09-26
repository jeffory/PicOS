// hardware/sync.h stub for simulator
//
// Default build: no-op spinlock primitives. http.h/tcp.h/toast.c include
// this header, but the simulator's own network layer (sim_http.c/sim_tcp.c)
// and toast never contend across threads in a way these locks protect.
//
// SIM_FIRMWARE_NET build (PICODECK_SIM_FIRMWARE_NET): the firmware's
// src/drivers/http.c, tcp.c and wifi.c run with Core 0 and Core 1 as two
// host threads, so the spinlocks they take around the Core 0/Core 1 shared
// ring buffers and pending flags must really exclude. Each hardware spinlock
// number maps to a pthread mutex (simulator/net/sim_net_shim.c), which
// ThreadSanitizer understands.

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef PICODECK_SIM_FIRMWARE_NET

#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_spin_lock spin_lock_t;

int spin_lock_claim_unused(bool required);
void spin_lock_unclaim(int lock_num);
spin_lock_t *spin_lock_instance(int lock_num);
uint32_t spin_lock_blocking(spin_lock_t *lock);
void spin_unlock(spin_lock_t *lock, uint32_t saved_irq);

#ifdef __cplusplus
}
#endif

// Data memory barrier (http_free orders Core 1's writes before freeing).
#define __dmb() atomic_thread_fence(memory_order_seq_cst)

#else  // !PICODECK_SIM_FIRMWARE_NET

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

#endif  // PICODECK_SIM_FIRMWARE_NET
