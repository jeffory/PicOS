#include "config.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
#include "flat_json.h"

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

        // Read key (escaped by config_save; over-long keys are truncated
        // and the rest skipped)
        char key[CONFIG_KEY_MAX];
        p = flat_json_read_string(p, key, sizeof(key));
        if (!*p) break;

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
        p = flat_json_read_string(p, val, sizeof(val));

        // Skip internal metadata key
        if (key[0] != '\0') {
            memcpy(s_entries[s_count].key, key, sizeof(key));  // NUL-terminated
            memcpy(s_entries[s_count].val, val, sizeof(val));
            s_count++;
        }
    }

    umm_free(json);
    printf("Config: loaded %d entries from %s\n", s_count, CONFIG_PATH);
    return true;
}

bool config_save(void) {
    // Capacity formula accounts for worst-case JSON escaping:
    //   - Each key char can expand to 2 bytes (e.g. '\' → "\\")    → 2*CONFIG_KEY_MAX
    //   - Each val char can expand to 2 bytes                       → 2*CONFIG_VAL_MAX
    //   - Per-entry overhead: "":""[,]                              → 8 bytes
    //   - Outer braces + null terminator                            → 4 bytes
    // So the estimate is tight but always sufficient for any key/value content.
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
