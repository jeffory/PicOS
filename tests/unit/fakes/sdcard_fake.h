// In-memory SD card for host unit tests: the subset of src/drivers/sdcard.h
// that config.c / appconfig.c / sound code use, backed by a small file table.
#pragma once

#include <stdbool.h>
#include <stddef.h>

void        sdfake_reset(void);
void        sdfake_put(const char *path, const char *data, size_t len);
// Contents of path (NUL-terminated) or NULL; *len gets its size.
const char *sdfake_get(const char *path, size_t *len);
bool        sdfake_dir_exists(const char *path);
// Make the next sdcard_fwrite calls write at most `limit` bytes (-1 = off).
void        sdfake_limit_writes(int limit);
