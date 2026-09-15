#pragma once
// Font slot table: 0..3 are the compiled-in fonts, 4..11 hold fonts loaded
// from .pfn files on the SD card. The launcher calls font_registry_unload_all
// around every app so loaded fonts never outlive their app.
#include "font.h"

#define FONT_REGISTRY_BUILTIN 4
#define FONT_REGISTRY_SLOTS   12

// Returns NULL for ids that are out of range or not currently loaded.
const pc_font_t *font_registry_get(int id);

// Read `path` (absolute SD path) into PSRAM and validate it. Returns the
// new slot id (>= FONT_REGISTRY_BUILTIN) or -1 (missing file, bad image,
// no free slot); the reason is printed to the serial log.
int font_registry_load(const char *path);

// Free one loaded slot. No-op for built-ins and empty slots.
void font_registry_unload(int id);

// Free every loaded slot.
void font_registry_unload_all(void);
