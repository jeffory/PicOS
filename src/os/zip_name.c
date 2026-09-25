#include "zip_name.h"

#include <stddef.h>
#include <string.h>

bool zip_entry_name_valid(const char *name) {
    if (!name || !name[0]) return false;

    size_t len = strlen(name);
    if (len > ZIP_MAX_NAME) return false;
    if (name[0] == '/') return false;  // absolute path

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F) return false;  // control chars
        if (c == '\\') return false;              // backslash separators
        if (c == ':') return false;               // drive prefixes / ADS
    }

    // Component-wise check: reject empty, "." and ".." components. A trailing
    // '/' marks a directory entry and is allowed. Deliberate behaviour change
    // from the old substring-based `strstr(name, "..")` check: a name like
    // "foo..bar" is ACCEPTED here — only exact "." / ".." path components are
    // traversal hazards.
    const char *p = name;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        if (clen == 0) return false;  // "//" or leading '/' (already rejected)
        if (clen == 1 && p[0] == '.') return false;
        if (clen == 2 && p[0] == '.' && p[1] == '.') return false;
        if (!slash) break;
        p = slash + 1;
        if (*p == '\0') break;  // trailing '/' → directory entry, done
    }
    return true;
}
