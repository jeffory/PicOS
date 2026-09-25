#pragma once
#include <stdbool.h>

#include "os.h"  // pczip_t / pczip_stat_t

// ZIP archive helpers shared by the firmware g_api.zip vtable and the
// simulator's Unicorn trampolines. All access goes through the hardened
// seek-based engine in zip_util.c.
// Returns true on success.
bool zip_archive_extract(const char *zip_path, const char *dest_dir);

// Returns number of files in the archive, or -1 on error.
int zip_archive_list(const char *zip_path);

// --- Read-in-place handles (API version 5) ---------------------------------
// Static pool of ZIP_ARCHIVE_MAX_OPEN readers; open fails when exhausted.
#define ZIP_ARCHIVE_MAX_OPEN 4

pczip_t zip_archive_open(const char *zip_path);
void    zip_archive_close(pczip_t z);
int     zip_archive_num_entries(pczip_t z);
int     zip_archive_locate(pczip_t z, const char *name);
bool    zip_archive_stat_index(pczip_t z, int idx, pczip_stat_t *out);
int     zip_archive_read(pczip_t z, int idx, void *buf, uint32_t buf_cap);
bool    zip_archive_extract_entry(pczip_t z, int idx, const char *dest_path);

// Force-close every open handle. Called by the launcher after an app exits
// so a leaked handle cannot exhaust the pool for the next app.
void    zip_archive_close_all(void);
