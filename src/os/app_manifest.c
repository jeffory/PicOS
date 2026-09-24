#include "app_manifest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// All scanning is bounded by [p, end); end is json + len or the first NUL.

static const char *find_bytes(const char *p, const char *end, const char *s,
                              size_t n) {
    for (; p && (size_t)(end - p) >= n; p++) {
        if (memcmp(p, s, n) == 0)
            return p;
    }
    return NULL;
}

// Next occurrence of "key" (with its quotes) at or after p; returns the
// position just past the closing quote, or NULL.
static const char *find_key(const char *p, const char *end, const char *key) {
    char search[64];
    int n = snprintf(search, sizeof(search), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(search))
        return NULL;
    const char *hit = find_bytes(p, end, search, (size_t)n);
    return hit ? hit + n : NULL;
}

static const char *skip_sep(const char *q, const char *end) {
    while (q < end && (*q == ' ' || *q == ':' || *q == '\t' || *q == '\r' ||
                       *q == '\n'))
        q++;
    return q;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode the string body starting just after its opening quote.
static void read_string(const char *q, const char *end, char *out,
                        size_t out_len) {
    size_t i = 0;
    while (q < end && *q != '"' && i + 1 < out_len) {
        char c = *q++;
        if (c == '\\' && q < end) {
            char e = *q++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                uint32_t cp = 0;
                int k = 0;
                for (; k < 4 && q < end; k++, q++) {
                    int h = hex_val(*q);
                    if (h < 0)
                        break;
                    cp = cp * 16u + (uint32_t)h;
                }
                c = (k == 4 && cp > 0 && cp < 0x80) ? (char)cp : '?';
                break;
            }
            default: c = e; break;  // \" \\ \/ and anything else literally
            }
        }
        out[i++] = c;
    }
    out[i] = '\0';
}

// String value of "key".  A match whose value is not a string (e.g. the key
// text appearing inside another value) is skipped and the search continues.
static bool json_get_string(const char *json, const char *end,
                            const char *key, char *out, size_t out_len) {
    const char *p = json;
    while ((p = find_key(p, end, key)) != NULL) {
        const char *q = skip_sep(p, end);
        if (q < end && *q == '"') {
            read_string(q + 1, end, out, out_len);
            return true;
        }
    }
    return false;
}

// Integer value of the first "key" (atoi semantics: leading '-' allowed,
// stops at the first non-digit, no digits = 0).
static bool json_get_int(const char *json, const char *end, const char *key,
                         uint32_t *out) {
    const char *p = find_key(json, end, key);
    if (!p)
        return false;
    p = skip_sep(p, end);
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) {
        neg = (*p == '-');
        p++;
    }
    uint32_t v = 0;
    while (p < end && *p >= '0' && *p <= '9')
        v = v * 10u + (uint32_t)(*p++ - '0');
    *out = neg ? (uint32_t)(0u - v) : v;
    return true;
}

// "requirement" (quoted) inside the "requirements" array.
static bool json_has_requirement(const char *json, const char *end,
                                 const char *requirement) {
    const char *p = find_key(json, end, "requirements");
    if (!p)
        return false;
    while (p < end && *p != '[')
        p++;
    if (p >= end)
        return false;
    const char *close = p;
    while (close < end && *close != ']')
        close++;
    if (close >= end)
        return false;  // unterminated array
    char search[96];
    int n = snprintf(search, sizeof(search), "\"%s\"", requirement);
    if (n <= 0 || (size_t)n >= sizeof(search))
        return false;
    return find_bytes(p, close, search, (size_t)n) != NULL;
}

static size_t bounded_len(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static void copy_str(char *dst, size_t n, const char *src) {
    snprintf(dst, n, "%s", src ? src : "");
}

void app_manifest_parse(const char *json, size_t len, const char *dir_name,
                        app_entry_t *app) {
    const char *end = json ? json + bounded_len(json, len) : json;

    if (!json || !json_get_string(json, end, "id", app->id, sizeof(app->id)))
        snprintf(app->id, sizeof(app->id), "local.%s", dir_name);
    if (!json || !json_get_string(json, end, "name", app->name, sizeof(app->name)))
        copy_str(app->name, sizeof(app->name), dir_name);
    if (!json || !json_get_string(json, end, "description", app->description,
                                  sizeof(app->description)))
        app->description[0] = '\0';
    if (!json || !json_get_string(json, end, "version", app->version,
                                  sizeof(app->version)))
        copy_str(app->version, sizeof(app->version), "1.0");

    app->has_root_filesystem =
        json && json_has_requirement(json, end, "root-filesystem");
    app->has_http = json && json_has_requirement(json, end, "http");
    app->has_audio = json && json_has_requirement(json, end, "audio");
    app->system_clock_khz = 0;
    app->min_psram_kb = 0;
    if (json) {
        json_get_int(json, end, "system_clock_khz", &app->system_clock_khz);
        json_get_int(json, end, "min_psram_kb", &app->min_psram_kb);
    }
    if (!json || !json_get_string(json, end, "category", app->category,
                                  sizeof(app->category)))
        app->category[0] = '\0';
}

void app_manifest_defaults(const char *dir_name, app_entry_t *app) {
    snprintf(app->id, sizeof(app->id), "local.%s", dir_name);
    copy_str(app->name, sizeof(app->name), dir_name);
    app->description[0] = '\0';
    copy_str(app->version, sizeof(app->version), "?");
    app->category[0] = '\0';
    app->has_root_filesystem = false;
    app->has_http = false;
    app->has_audio = false;
    app->system_clock_khz = 0;
    app->min_psram_kb = 0;
}

bool app_manifest_id_valid(const char *id) {
    if (!id || !id[0])
        return false;
    size_t n = bounded_len(id, sizeof(((app_entry_t *)0)->id));
    if (n >= sizeof(((app_entry_t *)0)->id))
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    if (strcmp(id, ".") == 0 || strstr(id, ".."))
        return false;
    return true;
}
