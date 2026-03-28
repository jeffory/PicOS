#pragma once

#include <stdint.h>
#include <stdbool.h>

// System-wide toast notification queue.
// Thread-safe: toast_push() can be called from any core.
// toast_draw() must be called from Core 0 only (display access).

#define TOAST_ICON_NONE  0
#define TOAST_ICON_WIFI  1
#define TOAST_ICON_ERROR 2
#define TOAST_ICON_INFO  3

// Initialize the toast subsystem. Call once at boot before Core 1 launch.
void toast_init(void);

// Push a toast message to the system queue. Thread-safe (spinlock protected).
// Message is copied (up to 63 chars). Auto-dismissed after ~3 seconds.
void toast_push(const char *msg, uint8_t icon);

// Draw the active toast (if any) into the current framebuffer.
// Returns true if a toast was drawn. Called from Core 0 render path.
bool toast_draw(void);
