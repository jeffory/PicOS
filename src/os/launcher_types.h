#pragma once
#include <stdbool.h>

// ── App types ─────────────────────────────────────────────────────────────────

typedef enum {
    APP_TYPE_LUA    = 0,  // main.lua present
    APP_TYPE_NATIVE = 1,  // main.elf present (C / TinyGo PIE binary)
} app_type_t;

// ── App discovery record ──────────────────────────────────────────────────────

typedef struct {
    char id[80];           // Reverse-DNS app ID (e.g. "com.picos.editor")
    char name[64];         // Display name from app.json
    char path[128];        // Full path to app directory ("/apps/foo")
    char description[128]; // Short description from app.json
    char version[16];
    app_type_t type;               // Detected app type
    bool has_root_filesystem;      // "root-filesystem" requirement
    bool has_http;                 // "http" requirement
    bool has_audio;                // "audio" requirement
} app_entry_t;
