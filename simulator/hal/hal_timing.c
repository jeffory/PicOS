// HAL Timing - SDL2 Implementation

#include "hal_timing.h"
#include <SDL2/SDL.h>

static int g_debug_mode = 0;
static float s_time_multiplier = 1.0f;

void hal_set_time_multiplier(float m) { s_time_multiplier = m < 0.0f ? 0.0f : m; }
float hal_get_time_multiplier(void) { return s_time_multiplier; }

void hal_timing_init(void) {
    // SDL timer is initialized with SDL_Init
}

uint32_t hal_get_time_ms(void) {
    return SDL_GetTicks();
}

uint64_t hal_get_time_us(void) {
    return (uint64_t)SDL_GetTicks() * 1000ULL;
}

void hal_sleep_ms(uint32_t ms) {
    if (s_time_multiplier <= 0.0f) return;  // 0 = paused (skip delay)
    SDL_Delay((uint32_t)(ms / s_time_multiplier));
}

void hal_sleep_us(uint64_t us) {
    if (us < 100) {
        // Busy-wait for sub-100μs precision (max 99μs spin)
        uint64_t start = SDL_GetPerformanceCounter();
        uint64_t freq = SDL_GetPerformanceFrequency();
        uint64_t target = start + (us * freq) / 1000000ULL;
        while (SDL_GetPerformanceCounter() < target) {
            // Busy wait
        }
    } else {
        SDL_Delay((uint32_t)(us / 1000));
    }
}

void hal_set_debug_mode(int enabled) {
    g_debug_mode = enabled;
}

int hal_get_debug_mode(void) {
    return g_debug_mode;
}
