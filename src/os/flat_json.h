#pragma once

// String reader shared by the flat {"key":"value"} stores (config.c,
// appconfig.c).  Header-only so neither store gains a dependency.

#include <stddef.h>

// p points just past an opening '"'.  Decodes the string into out (at most
// out_len - 1 chars, always NUL-terminated; longer strings are truncated)
// and returns the position just past the closing '"' (or at the NUL if the
// string is unterminated), so an over-long string never desynchronises the
// caller.  Escapes: \n \t \r \b \f decode; \X is X (covers \" \\ \/).
static inline const char *flat_json_read_string(const char *p, char *out,
                                                size_t out_len) {
    size_t i = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            c = *p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            default: break;
            }
        }
        if (i + 1 < out_len)
            out[i++] = c;
    }
    if (out_len)
        out[i] = '\0';
    return *p == '"' ? p + 1 : p;
}
