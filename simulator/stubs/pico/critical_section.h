// pico/critical_section.h stub
#ifndef PICO_CRITICAL_SECTION_H
#define PICO_CRITICAL_SECTION_H

#include "pico/mutex.h"
#include <stdint.h>

typedef mutex_t critical_section_t;

static inline void critical_section_init(critical_section_t* cs) {
    mutex_init(cs);
}

static inline void critical_section_deinit(critical_section_t* cs) {
    mutex_deinit(cs);
}

static inline void critical_section_enter_blocking(critical_section_t* cs) {
    mutex_enter_blocking(cs);
}

static inline void critical_section_exit(critical_section_t* cs) {
    mutex_exit(cs);
}

// Save/restore interrupt state (no-op on PC)
typedef uint32_t critical_section_saved_state_t;

static inline critical_section_saved_state_t critical_section_enter_blocking_and_save_state(critical_section_t* cs) {
    critical_section_enter_blocking(cs);
    return 0;
}

static inline void critical_section_exit_and_restore_state(critical_section_t* cs, critical_section_saved_state_t state) {
    (void)state;
    critical_section_exit(cs);
}

static inline uint32_t critical_section_is_initialized(critical_section_t* cs) {
    return 1;  // Always initialized on simulator
}

// Inline critical sections (just mutex for simulator)
static inline void critical_section_enter_blocking_inline(critical_section_t* cs) {
    critical_section_enter_blocking(cs);
}

static inline void critical_section_exit_inline(critical_section_t* cs) {
    critical_section_exit(cs);
}

#endif // PICO_CRITICAL_SECTION_H
