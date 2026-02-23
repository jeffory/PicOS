#include "config.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define CONFIG_PATH "/system/config.json"

// ── In-memory store ───────────────────────────────────────────────────────────

typedef struct {
    char key[CONFIG_KEY_MAX];
    char val[CONFIG_VAL_MAX];
} config_entry_t;

static config_entry_t s_entries[CONFIG_MAX_ENTRIES];
static int            s_count = 0;

// ── JSON helpers ──────────────────────────────────────────────────────────────

// Extract the value for `key` from a flat JSON object string.
// Writes at most out_len-1 bytes to `out` and null-terminates.
// Returns true on success.
static bool json_get_string(const char *json, const char *key,
                             char *out, int out_len) {
    // Build search pattern: "key":
    char search[CONFIG_KEY_MAX + 4];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *p = strstr(json, search);
    if (!p) return false;

    // Advance past "key"
    p += strlen(search);

    // Skip whitespace and ':'
    while (*p == ' ' || *p == '\t' || *p == ':') p++;

    // Expect opening quote
    if (*p != '"') return false;
    p++;  // skip opening "

    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        // Handle basic escape sequences
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n':  out[i++] = '\n'; break;
                case 't':  out[i++] = '\t'; break;
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return true;
}

// ── Public API ─────────────────────────────────────────────────────────────────

bool config_load(void) {
    s_count = 0;

    int len = 0;
    char *json = sdcard_read_file(CONFIG_PATH, &len);
    if (!json) {
        // File doesn't exist yet — empty config is fine
        return false;
    }

    // Walk the JSON string looking for "key":"value" pairs.
    // We keep a simple cursor that scans for opening quotes.
    const char *p = json;
    while (s_count < CONFIG_MAX_ENTRIES) {
        // Find next '"'
        p = strchr(p, '"');
        if (!p) break;
        p++;  // skip opening "

        // Read key
        char key[CONFIG_KEY_MAX];
        int ki = 0;
        while (*p && *p != '"' && ki < (int)sizeof(key) - 1)
            key[ki++] = *p++;
        key[ki] = '\0';
        if (!*p) break;
        p++;  // skip closing "

        // Skip whitespace and ':'
        while (*p == ' ' || *p == '\t' || *p == ':') p++;

        // Expect value '"'
        if (*p != '"') {
            // Not a string value — skip to next ','
            p = strchr(p, ',');
            if (!p) break;
            continue;
        }
        p++;  // skip opening "

        // Read value
        char val[CONFIG_VAL_MAX];
        int vi = 0;
        while (*p && *p != '"' && vi < (int)sizeof(val) - 1) {
            if (*p == '\\' && *(p + 1)) {
                p++;
                switch (*p) {
                    case 'n':  val[vi++] = '\n'; break;
                    case 't':  val[vi++] = '\t'; break;
                    case '"':  val[vi++] = '"';  break;
                    case '\\': val[vi++] = '\\'; break;
                    default:   val[vi++] = *p;   break;
                }
            } else {
                val[vi++] = *p;
            }
            p++;
        }
        val[vi] = '\0';
        if (*p == '"') p++;  // skip closing "

        // Skip internal metadata key
        if (key[0] != '\0') {
            strncpy(s_entries[s_count].key, key, CONFIG_KEY_MAX - 1);
            strncpy(s_entries[s_count].val, val, CONFIG_VAL_MAX - 1);
            s_count++;
        }
    }

    umm_free(json);
    printf("Config: loaded %d entries from %s\n", s_count, CONFIG_PATH);
    return true;
}

bool config_save(void) {
    int  cap = s_count * (2 * (CONFIG_KEY_MAX + CONFIG_VAL_MAX) + 8) + 4;
    char *buf = (char *)umm_malloc(cap);
    if (!buf) return false;
    int  pos = 0;

    buf[pos++] = '{';
    for (int i = 0; i < s_count; i++) {
        if (i > 0) buf[pos++] = ',';

        buf[pos++] = '"';
        for (const char *k = s_entries[i].key; *k; k++) {
            if (*k == '"' || *k == '\\') buf[pos++] = '\\';
            buf[pos++] = *k;
        }
        buf[pos++] = '"';
        buf[pos++] = ':';
        buf[pos++] = '"';
        for (const char *v = s_entries[i].val; *v; v++) {
            if (*v == '"' || *v == '\\') buf[pos++] = '\\';
            else if (*v == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n'; continue; }
            else if (*v == '\t') { buf[pos++] = '\\'; buf[pos++] = 't'; continue; }
            buf[pos++] = *v;
        }
        buf[pos++] = '"';
    }
    buf[pos++] = '}';
    buf[pos]   = '\0';

    sdfile_t f = sdcard_fopen(CONFIG_PATH, "w");
    if (!f) {
        printf("Config: failed to open %s for writing\n", CONFIG_PATH);
        umm_free(buf);
        return false;
    }
    int written = sdcard_fwrite(f, buf, pos);
    sdcard_fclose(f);
    umm_free(buf);

    if (written != pos) {
        printf("Config: write truncated (%d/%d)\n", written, pos);
        return false;
    }
    printf("Config: saved %d entries to %s\n", s_count, CONFIG_PATH);
    return true;
}

const char *config_get(const char *key) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0)
            return s_entries[i].val;
    }
    return NULL;
}

void config_set(const char *key, const char *value) {
    if (!key || !key[0]) return;

    // Remove key if value is NULL or empty
    if (!value || !value[0]) {
        for (int i = 0; i < s_count; i++) {
            if (strcmp(s_entries[i].key, key) == 0) {
                // Shift remaining entries left
                for (int j = i; j < s_count - 1; j++)
                    s_entries[j] = s_entries[j + 1];
                s_count--;
                return;
            }
        }
        return;
    }

    // Update existing entry
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0) {
            strncpy(s_entries[i].val, value, CONFIG_VAL_MAX - 1);
            s_entries[i].val[CONFIG_VAL_MAX - 1] = '\0';
            return;
        }
    }

    // Insert new entry
    if (s_count >= CONFIG_MAX_ENTRIES) return;
    strncpy(s_entries[s_count].key, key,   CONFIG_KEY_MAX - 1);
    strncpy(s_entries[s_count].val, value, CONFIG_VAL_MAX - 1);
    s_entries[s_count].key[CONFIG_KEY_MAX - 1] = '\0';
    s_entries[s_count].val[CONFIG_VAL_MAX - 1] = '\0';
    s_count++;
}
