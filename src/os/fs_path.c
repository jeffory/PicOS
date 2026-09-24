#include "fs_path.h"

#include <string.h>
#include <strings.h>

// FatFS names are case-insensitive, so every prefix test is too: "/DATA/X"
// is the same directory as "/data/x", and an allow-list compared
// case-sensitively would only refuse legitimate spellings.

// path is dir itself or lies beneath it.
static bool path_under(const char *path, const char *dir) {
    if (!dir || !dir[0])
        return false;
    size_t n = strlen(dir);
    return strncasecmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

bool fs_path_allowed(const char *path, bool write, const char *app_dir,
                     const char *data_dir, bool root_fs) {
    if (!path || path[0] != '/')
        return false;  // require absolute paths
    if (strstr(path, ".."))
        return false;  // reject traversal

    // Read-only shared libraries for every app.
    if (!write && strncasecmp(path, "/system/lib/", 12) == 0)
        return true;

    if (root_fs)
        return true;

    bool in_data = path_under(path, data_dir);
    if (write)
        return in_data;
    return in_data || path_under(path, app_dir);
}

bool fs_name_valid(const char *name, size_t len, size_t max_len) {
    if (!name || len == 0 || len > max_len || name[0] == '.')
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok)
            return false;
        if (c == '.' && i + 1 < len && name[i + 1] == '.')
            return false;
    }
    return true;
}
