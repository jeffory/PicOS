#pragma once
#include <stdbool.h>

// ZIP archive helpers shared by the firmware g_api.zip vtable and the
// simulator's Unicorn trampolines. Implemented with miniz + the sdcard layer.
// Returns true on success.
bool zip_archive_extract(const char *zip_path, const char *dest_dir);

// Returns number of files in the archive, or -1 on error.
int zip_archive_list(const char *zip_path);
