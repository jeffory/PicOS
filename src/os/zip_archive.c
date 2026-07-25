#include "zip_archive.h"

#include <stdio.h>

#include "zip_util.h"

#ifndef PICOS_SIMULATOR
#include "hardware/watchdog.h"
#endif

// Shared g_api.zip / simulator-trampoline entry points. Everything goes
// through the hardened seek-based engine in zip_util.c: the archive streams
// from SD (no whole-file PSRAM copy), entry names are validated against
// traversal, parent directories are created, and entry-count / total-size
// caps are enforced.

static bool zip_archive_progress(int done, int total, const char *name,
                                 void *user) {
    (void)done; (void)total; (void)name; (void)user;
#ifndef PICOS_SIMULATOR
    // Extraction can outlast the 10s watchdog window and nothing else feeds
    // it while we're in here.
    watchdog_update();
#endif
    return true;
}

bool zip_archive_extract(const char *zip_path, const char *dest_dir) {
    zip_reader_t zr;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&zr, zip_path, err)) {
        printf("[ZIP] %s: %s\n", zip_path, err);
        return false;
    }
    zip_extract_result_t result;
    bool ok = zip_reader_extract_all(&zr, dest_dir, NULL,
                                     zip_archive_progress, NULL, &result, err);
    zip_reader_close(&zr);
    if (!ok)
        printf("[ZIP] extract %s -> %s failed: %s\n", zip_path, dest_dir, err);
    return ok;
}

int zip_archive_list(const char *zip_path) {
    zip_reader_t zr;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&zr, zip_path, err))
        return -1;
    int n = zip_reader_num_entries(&zr);
    zip_reader_close(&zr);
    return n;
}
