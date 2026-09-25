#include "dev_ops.h"
#include "dev_commands.h"
#include "drivers/sdcard.h"
#include "os/launcher.h"
#include "os/zip_util.h"
#include "hardware/watchdog.h"

#include <stdio.h>
#include <string.h>

bool dev_op_exit(char *reply, size_t n) {
    dev_commands_set_exit();
    const char *app = launcher_get_running_app_name();
    if (!app || !app[0]) {
        snprintf(reply, n, "Error: exit: no app running");
        return false;
    }
    snprintf(reply, n, "Exit requested: %s", app);
    return true;
}

// Periodic status lines for the host tool and a watchdog feed: extraction of
// a big archive easily outlasts the 10s window and nothing else runs on
// Core 0 while we're in here.
static bool dev_unzip_progress(int done, int total, const char *name,
                               void *user) {
    (void)name; (void)user;
    watchdog_update();
    if (done % 25 == 0 || done == total)
        printf("[DEV] UNZIP %d/%d\n", done, total);
    return true;
}

bool dev_op_unzip(char *args, char *reply, size_t n) {
    // Blocks Core 0 for the duration (same contract as putb64: audio decoded
    // on Core 1 will starve). The engine validates entry names.
    char *zip_path = args;
    char *dest = strchr(zip_path, ' ');
    if (!dest || zip_path[0] != '/' || dest[1] != '/') {
        snprintf(reply, n, "Usage: unzip /path/to.zip /dest/dir");
        return false;
    }
    *dest++ = '\0';
    zip_reader_t zr;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&zr, zip_path, err)) {
        snprintf(reply, n, "Error: unzip failed: %s", err);
        return false;
    }
    zip_extract_result_t result;
    bool ok = zip_reader_extract_all(&zr, dest, NULL, dev_unzip_progress, NULL,
                                     &result, err);
    zip_reader_close(&zr);
    if (!ok) {
        snprintf(reply, n, "Error: unzip failed: %s", err);
        return false;
    }
    snprintf(reply, n, "Unzipped %d files (%d skipped)", result.files_done,
             result.skipped_names);
    return true;
}

bool dev_op_rm(const char *path, char *reply, size_t n) {
    if (path[0] != '/' || strcmp(path, "/") == 0) {
        snprintf(reply, n, "Usage: rm /absolute/path (file or directory)");
        return false;
    }
    if (sdcard_delete(path) || sdcard_delete_recursive(path)) {
        snprintf(reply, n, "Deleted: %s", path);
        return true;
    }
    snprintf(reply, n, "Error: rm failed: %s", path);
    return false;
}
