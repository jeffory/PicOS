#pragma once

#include <stdint.h>
#include <stdbool.h>

// Save the current framebuffer as a BMP file to /screenshots/shotNNN.bmp.
// The file number increments automatically (001–999).
// Briefly displays a "Screenshot saved" overlay after writing.
void screenshot_save(void);

// Schedule a screenshot to be taken after delay_ms milliseconds.
void screenshot_schedule(uint32_t delay_ms);

// Returns true (and resets the schedule) if the delay has elapsed.
// The caller is responsible for actually saving the screenshot at the
// right time (e.g. after the next display_flush).
bool screenshot_check_scheduled(void);
