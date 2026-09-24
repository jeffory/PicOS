// HAL Timing - SDL2 Implementation

#ifndef HAL_TIMING_H
#define HAL_TIMING_H

#include <stdbool.h>
#include <stdint.h>

// Initialize timing subsystem
void hal_timing_init(void);

// Get time in milliseconds
uint32_t hal_get_time_ms(void);

// Get time in microseconds
uint64_t hal_get_time_us(void);

// Sleep for milliseconds
void hal_sleep_ms(uint32_t ms);

// Sleep for microseconds
void hal_sleep_us(uint64_t us);

// Debug mode
void hal_set_debug_mode(int enabled);
int hal_get_debug_mode(void);

// Deterministic timing (0 = pause, 1 = realtime, >1 = fast-forward).
// Wall clock: scales hal_sleep_ms()/hal_sleep_us() delays only.
// Virtual clock: the speed of virtual time (see hal_timing.c).
void hal_set_time_multiplier(float m);
float hal_get_time_multiplier(void);

// Virtual clock (--virtual-time). Call hal_timing_set_virtual(true) before
// hal_timing_init(); the thread that calls hal_timing_init() owns the clock.
void hal_timing_set_virtual(bool on);
bool hal_timing_is_virtual(void);

// Advance the virtual clock by `ms` and wake a paused owner sleep. Returns
// false (and does nothing) on the wall clock. *now_ms gets the new time.
bool hal_time_step(uint32_t ms, uint64_t *now_ms);

#endif // HAL_TIMING_H
