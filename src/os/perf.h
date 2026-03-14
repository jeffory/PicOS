#ifndef PERF_H
#define PERF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the performance monitoring system.
void perf_init(void);

// Start timing a frame. Call at the beginning of the game loop.
void perf_begin_frame(void);

// End timing a frame and update FPS calculation. Call at the end of the game loop.
void perf_end_frame(void);

// Get current FPS (averaged over recent frames).
int perf_get_fps(void);

// Get last frame time in milliseconds.
uint32_t perf_get_frame_time(void);

// Set target FPS for automatic frame pacing (0 = no limit).
void perf_set_target_fps(uint32_t fps);

#ifdef __cplusplus
}
#endif

#endif // PERF_H
