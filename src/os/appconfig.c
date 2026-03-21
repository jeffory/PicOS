#include "appconfig.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define APPCONFIG_PATH_LEN 64

static char s_app_id[64] = {0};
static char s_config_path[APPCONFIG_PATH_LEN] = {0};

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
    
    strncpy(s_app_id, app_id, sizeof(s_app_id) - 1);
    s_app_id[sizeof(s_app_id) - 1] = '\0';
    
    snprintf(s_config_path, sizeof(s_config_path), "/data/%s/config.json", app_id);
    
    s_count = 0;
    
    int len = 0;
    char *json = sdcard_read_file(s_config_path, &len);
    if (!json) {
        printf("[APPCONFIG] No config file at %s (first run?)\n", s_config_path);
        return false;
    }
    
    const char *p = json;
    while (s_count < APPCONFIG_MAX_ENTRIES) {
        p = strchr(p, '"');
        if (!p) break;
        p++;
        
        char key[APPCONFIG_KEY_MAX];
        int ki = 0;
        while (*p && *p != '"' && ki < (int)sizeof(key) - 1)
            key[ki++] = *p++;
        key[ki] = '\0';
        if (!*p) break;
        p++;
        
        while (*p == ' ' || *p == '\t' || *p == ':') p++;
        
        if (*p != '"') {
            p = strchr(p, ',');
            if (!p) break;
            continue;
        }
        p++;
        
        char val[APPCONFIG_VAL_MAX];
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
        if (*p == '"') p++;
        
        if (key[0] != '\0') {
            strncpy(s_entries[s_count].key, key, APPCONFIG_KEY_MAX - 1);
            strncpy(s_entries[s_count].val, val, APPCONFIG_VAL_MAX - 1);
            s_entries[s_count].key[APPCONFIG_KEY_MAX - 1] = '\0';
            s_entries[s_count].val[APPCONFIG_VAL_MAX - 1] = '\0';
            s_count++;
        }
    }
    
    umm_free(json);
    printf("[APPCONFIG] Loaded %d entries from %s\n", s_count, s_config_path);
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
    
    sdfile_t f = sdcard_fopen(s_config_path, "w");
    if (!f) {
        printf("[APPCONFIG] ERROR: Failed to open %s for writing\n", s_config_path);
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
    printf("[APPCONFIG] Saved %d entries to %s\n", s_count, s_config_path);
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
        appconfig_set(key, "");
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
    
    if (sdcard_fexists(s_config_path)) {
        sdcard_delete(s_config_path);
        printf("[APPCONFIG] Deleted config file: %s\n", s_config_path);
    }
    
    s_count = 0;
    return true;
}

const char *appconfig_get_app_id(void) {
    return s_app_id[0] ? s_app_id : NULL;
}
