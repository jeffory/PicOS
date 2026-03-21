#pragma once

#include <stdbool.h>

// =============================================================================
// Shared Config — /system/config.json
//
// A minimal flat key/value store backed by a JSON file on the SD card.
// Supports string values only. Maximum CONFIG_MAX_ENTRIES entries.
//
// JSON format: {"key1":"value1","key2":"value2"}
//
// Well-known keys:
//   "wifi_ssid"   — WiFi network name
//   "wifi_pass"   — WiFi password
//   "brightness"  — Display brightness (0-255, stored as decimal string)
// =============================================================================

#define CONFIG_MAX_ENTRIES  6
#define CONFIG_KEY_MAX      32
#define CONFIG_VAL_MAX      128

// Load /system/config.json from SD card into memory.
// Returns true if the file was read; false if missing (empty config is valid).
// Safe to call before the file exists.
bool        config_load(void);

// Write current in-memory config back to /system/config.json.
// Returns true on success.
bool        config_save(void);

// Return the value for key, or NULL if the key is not present.
const char *config_get(const char *key);

// Set or overwrite a string value.  A NULL or empty value removes the key.
// Silently does nothing if the store is full (CONFIG_MAX_ENTRIES reached).
void        config_set(const char *key, const char *value);
