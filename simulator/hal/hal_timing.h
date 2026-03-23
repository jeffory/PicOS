// HAL Timing - SDL2 Implementation

#ifndef HAL_TIMING_H
#define HAL_TIMING_H

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

// Deterministic timing (0 = pause, 1 = realtime, >1 = fast-forward)
void hal_set_time_multiplier(float m);
float hal_get_time_multiplier(void);

#endif // HAL_TIMING_H
