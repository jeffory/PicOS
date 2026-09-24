#include "appconfig.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"
#include "flat_json.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// app_entry_t.id is 80 bytes; the path is built on demand (no static copy)
// and sized for the longest id: "/data/" + 79 + "/config.json".
#define APPCONFIG_ID_MAX   80
#define APPCONFIG_PATH_LEN (6 + APPCONFIG_ID_MAX + 12)

static char s_app_id[APPCONFIG_ID_MAX] = {0};

static void config_path(char *out, size_t n) {
    snprintf(out, n, "/data/%s/config.json", s_app_id);
}

typedef struct {
    char key[APPCONFIG_KEY_MAX];
    char val[APPCONFIG_VAL_MAX];
} appconfig_entry_t;

static appconfig_entry_t s_entries[APPCONFIG_MAX_ENTRIES];
static int s_count = 0;

static bool ensure_data_dir(void) {
    char dir_path[APPCONFIG_PATH_LEN];
    snprintf(dir_path, sizeof(dir_path), "/data/%s", s_app_id);
    
    if (!sdcard_fexists(dir_path)) {
        printf("[APPCONFIG] Creating directory: %s\n", dir_path);
        if (!sdcard_mkdir(dir_path)) {
            printf("[APPCONFIG] ERROR: Failed to create directory %s\n", dir_path);
            return false;
        }
    }
    return true;
}

bool appconfig_load(const char *app_id) {
    if (!app_id || !app_id[0]) {
        printf("[APPCONFIG] ERROR: No app_id provided\n");
        return false;
    }
    
    s_count = 0;
    if (strlen(app_id) >= sizeof(s_app_id)) {
        // Too long to be a launcher id; never truncate into another path.
        printf("[APPCONFIG] ERROR: app_id too long\n");
        s_app_id[0] = '\0';
        return false;
    }
    strcpy(s_app_id, app_id);

    char path[APPCONFIG_PATH_LEN];
    config_path(path, sizeof(path));
    
    int len = 0;
    char *json = sdcard_read_file(path, &len);
    if (!json) {
        printf("[APPCONFIG] No config file at %s (first run?)\n", path);
        return false;
    }
    
    const char *p = json;
    while (s_count < APPCONFIG_MAX_ENTRIES) {
        p = strchr(p, '"');
        if (!p) break;
        p++;
        
        char key[APPCONFIG_KEY_MAX];
        p = flat_json_read_string(p, key, sizeof(key));
        if (!*p) break;
        
        while (*p == ' ' || *p == '\t' || *p == ':') p++;
        
        if (*p != '"') {
            p = strchr(p, ',');
            if (!p) break;
            continue;
        }
        p++;
        
        char val[APPCONFIG_VAL_MAX];
        p = flat_json_read_string(p, val, sizeof(val));

        if (key[0] != '\0') {
            memcpy(s_entries[s_count].key, key, sizeof(key));  // NUL-terminated
            memcpy(s_entries[s_count].val, val, sizeof(val));
            s_entries[s_count].key[APPCONFIG_KEY_MAX - 1] = '\0';
            s_entries[s_count].val[APPCONFIG_VAL_MAX - 1] = '\0';
            s_count++;
        }
    }
    
    umm_free(json);
    printf("[APPCONFIG] Loaded %d entries from %s\n", s_count, path);
    return true;
}

bool appconfig_save(void) {
    if (!s_app_id[0]) {
        printf("[APPCONFIG] ERROR: No app_id set, cannot save\n");
        return false;
    }
    
    if (!ensure_data_dir()) {
        return false;
    }
    
    int cap = s_count * (2 * (APPCONFIG_KEY_MAX + APPCONFIG_VAL_MAX) + 8) + 4;
    char *buf = (char *)umm_malloc(cap);
    if (!buf) {
        printf("[APPCONFIG] ERROR: OOM during save\n");
        return false;
    }
    
    int pos = 0;
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
    buf[pos] = '\0';
    
    char path[APPCONFIG_PATH_LEN];
    config_path(path, sizeof(path));
    sdfile_t f = sdcard_fopen(path, "w");
    if (!f) {
        printf("[APPCONFIG] ERROR: Failed to open %s for writing\n", path);
        umm_free(buf);
        return false;
    }
    int written = sdcard_fwrite(f, buf, pos);
    sdcard_fclose(f);
    umm_free(buf);
    
    if (written != pos) {
        printf("[APPCONFIG] ERROR: Write truncated (%d/%d)\n", written, pos);
        return false;
    }
    printf("[APPCONFIG] Saved %d entries to %s\n", s_count, path);
    return true;
}

const char *appconfig_get(const char *key, const char *fallback) {
    if (!key) return fallback;
    
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0)
            return s_entries[i].val;
    }
    return fallback;
}

void appconfig_set(const char *key, const char *value) {
    if (!key || !key[0]) return;
    
    if (!value || !value[0]) {
        // Delete the key if it exists
        for (int i = 0; i < s_count; i++) {
            if (strcmp(s_entries[i].key, key) == 0) {
                for (int j = i; j < s_count - 1; j++)
                    s_entries[j] = s_entries[j + 1];
                s_count--;
                return;
            }
        }
        return;
    }
    
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0) {
            strncpy(s_entries[i].val, value, APPCONFIG_VAL_MAX - 1);
            s_entries[i].val[APPCONFIG_VAL_MAX - 1] = '\0';
            return;
        }
    }
    
    if (s_count >= APPCONFIG_MAX_ENTRIES) {
        printf("[APPCONFIG] WARNING: Config full, cannot add %s\n", key);
        return;
    }
    
    strncpy(s_entries[s_count].key, key, APPCONFIG_KEY_MAX - 1);
    strncpy(s_entries[s_count].val, value, APPCONFIG_VAL_MAX - 1);
    s_entries[s_count].key[APPCONFIG_KEY_MAX - 1] = '\0';
    s_entries[s_count].val[APPCONFIG_VAL_MAX - 1] = '\0';
    s_count++;
}

void appconfig_clear(void) {
    s_count = 0;
    printf("[APPCONFIG] Cleared in-memory config\n");
}

bool appconfig_reset(void) {
    if (!s_app_id[0]) {
        printf("[APPCONFIG] ERROR: No app_id set, cannot reset\n");
        return false;
    }
    
    char path[APPCONFIG_PATH_LEN];
    config_path(path, sizeof(path));
    if (sdcard_fexists(path)) {
        sdcard_delete(path);
        printf("[APPCONFIG] Deleted config file: %s\n", path);
    }
    
    s_count = 0;
    return true;
}

void appconfig_unbind(void) {
    s_app_id[0] = '\0';
    s_count = 0;
}

const char *appconfig_get_app_id(void) {
    return s_app_id[0] ? s_app_id : NULL;
}
