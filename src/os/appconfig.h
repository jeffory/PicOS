#pragma once

#include <stdbool.h>

// =============================================================================
// App Config — /data/<APP_ID>/config.json
//
// Per-app configuration storage. Each app gets its own isolated config file
// stored in /data/<APP_ID>/config.json.
//
// Auto-creates the /data/<APP_ID>/ directory on first save if it doesn't exist.
//
// API (C):
//   appconfig_load(app_id)      — load from /data/<APP_ID>/config.json
//   appconfig_save()            — save to file
//   appconfig_get(key, default)— get value, returns default if not found
//   appconfig_set(key, value)  — set value
//   appconfig_clear()          — clear in-memory store
//   appconfig_reset()          — delete config file
// =============================================================================

#define APPCONFIG_MAX_ENTRIES  4
#define APPCONFIG_KEY_MAX      32
#define APPCONFIG_VAL_MAX      256

bool        appconfig_load(const char *app_id);
bool        appconfig_save(void);
const char *appconfig_get(const char *key, const char *fallback);
void        appconfig_set(const char *key, const char *value);
void        appconfig_clear(void);
bool        appconfig_reset(void);
const char *appconfig_get_app_id(void);
