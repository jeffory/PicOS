#pragma once

#include <stdint.h>
#include <stdbool.h>

// System-wide toast notification queue.
// Thread-safe: toast_push() can be called from any core.
// toast_draw() must be called from Core 0 only (display access).

// Toast style constants (controls background color)
#define TOAST_STYLE_INFO    0   // Default blue-gray
#define TOAST_STYLE_SUCCESS 1   // Green
#define TOAST_STYLE_WARNING 2   // Amber/orange
#define TOAST_STYLE_ERROR   3   // Red

// Initialize the toast subsystem. Call once at boot before Core 1 launch.
void toast_init(void);

// Push a toast message to the system queue. Thread-safe (spinlock protected).
// Message is copied (up to 63 chars). Auto-dismissed after ~3 seconds.
// style: TOAST_STYLE_INFO, _SUCCESS, _WARNING, or _ERROR.
void toast_push(const char *msg, uint8_t style);

// Draw the active toast (if any) into the current framebuffer.
// Returns true if a toast was drawn. Called from Core 0 render path.
bool toast_draw(void);
