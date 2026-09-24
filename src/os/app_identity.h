#pragma once

// The running app's identity and grants, owned by C.
//
// The runner (lua_runner.c / native_loader.c) calls app_identity_begin()
// before the app can execute anything — for Lua, before the bridge is
// registered — and app_identity_end() after the app has fully torn down.
// Every enforcement decision (the fs sandbox, the per-app config store,
// /data/<id> paths) reads this struct, never the APP_* Lua globals: those
// are copies for the app's convenience and the app may overwrite them.
//
// Stored in PSRAM (umm_malloc) — the SRAM heap has no room for it.  Later
// per-app bookkeeping (resource tracking) belongs here too.

#include <stdbool.h>

#include "launcher_types.h"

typedef struct {
    char id[80];             // app.json id, folded to lower case, validated
    char dir[128];           // "/apps/<dir>" (the bundle, read-only)
    char data_dir[96];       // "/data/<id>"  (read + write)
    char name[64];           // display name (truncated), for logs
    bool is_native;
    bool root_fs;            // "root-filesystem"
    bool http;               // "http"
    bool audio;              // "audio"
    char requirements[128];  // every requirement name, space-separated
} app_identity_t;

// Install *app as the running app.  Refuses (returns false, nothing
// installed) an id that fails app_manifest_id_valid() or when PSRAM is
// exhausted.
bool app_identity_begin(const app_entry_t *app);

// Clear the running app.
void app_identity_end(void);

// The running app, or NULL between apps.
const app_identity_t *app_identity_current(void);

// Did the running app declare requirement `name` (e.g. "sysconfig")?
// False when no app is running.
bool app_identity_has_requirement(const char *name);
