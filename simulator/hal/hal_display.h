// HAL Display - SDL2 Implementation
// Simulates the 320x320 ST7365P LCD display

#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

// Display dimensions (actual hardware resolution)
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 320

// Scale factor (3x = 960x960 window)
#ifndef DISPLAY_SCALE
#define DISPLAY_SCALE 3
#endif

// Window dimensions
#define WINDOW_WIDTH (DISPLAY_WIDTH * DISPLAY_SCALE)
#define WINDOW_HEIGHT (DISPLAY_HEIGHT * DISPLAY_SCALE)

// Initialize display subsystem
bool hal_display_init(const char* title);

// Shutdown display subsystem
void hal_display_shutdown(void);

// Get pointer to framebuffer (RGB565 format, 320x320)
uint16_t* hal_display_get_framebuffer(void);

// Present framebuffer to screen
void hal_display_present(void);

// Clear framebuffer to black
void hal_display_clear(void);

// Set a single pixel (for testing)
void hal_display_set_pixel(int x, int y, uint16_t color);

#endif // HAL_DISPLAY_H
