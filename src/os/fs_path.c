#include "fs_path.h"

#include <string.h>

// path is dir itself or lies beneath it.
static bool path_under(const char *path, const char *dir) {
    if (!dir || !dir[0])
        return false;
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

bool fs_path_allowed(const char *path, bool write, const char *app_dir,
                     const char *data_dir, bool root_fs) {
    if (!path || path[0] != '/')
        return false;  // require absolute paths
    if (strstr(path, ".."))
        return false;  // reject traversal

    // Read-only shared libraries for every app.
    if (!write && strncmp(path, "/system/lib/", 12) == 0)
        return true;

    if (root_fs)
        return true;

    bool in_data = path_under(path, data_dir);
    if (write)
        return in_data;
    return in_data || path_under(path, app_dir);
}
