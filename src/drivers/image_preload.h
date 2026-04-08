#pragma once
#include "image_api.h"
#include <stdbool.h>

// Async image preloading on Core 1.
// Single-slot: one preload at a time. New requests cancel the previous.

// Call once at startup (before core1 launch).
void image_preload_init(void);

// Core 0: submit a path for background decoding on Core 1.
// Cancels any pending/completed preload. Path is copied internally.
// Returns false if a decode is currently in progress (retry next frame).
bool image_preload_start(const char *path);

// Core 0: check if preloaded image is ready.
// Returns decoded image (caller owns it, must image_free()).
// Returns NULL if not ready or decode failed.
// If ready is non-NULL, sets *ready = true when decode finished (success or fail).
pc_image_t *image_preload_poll(bool *ready);

// Core 0: cancel any pending preload. Safe to call anytime.
void image_preload_cancel(void);

// Core 1: called each tick from core1_entry(). Does the actual decode.
void image_preload_update(void);
