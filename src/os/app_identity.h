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
// Stored in PSRAM (umm_malloc) — the SRAM heap has no room for it.  Per-app
// resource tracking lives here too (the open-file list below).

#include <stdbool.h>

#include "launcher_types.h"

// Upper bound on files one app can hold open.  FatFS itself allows 16
// (FF_FS_LOCK, shared with the OS), so the list never refuses first.
#define APP_FILES_MAX 32

// One handle an app holds through the API (app_res_*, below).
typedef struct {
    int kind;                // runner-defined class (native_res_kind_t)
    void *handle;
} app_res_t;

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
    int n_open_files;                    // app_files_* (below)
    void *open_files[APP_FILES_MAX];     // sdcard_fopen handles
    app_res_t *res;                      // app_res_* (below), umm_malloc'd
    int n_res, cap_res;
} app_identity_t;

// Install *app as the running app.  Refuses (returns false, nothing
// installed) an id that fails app_manifest_id_valid() or when PSRAM is
// exhausted.  Resets the per-app config store.
bool app_identity_begin(const app_entry_t *app);

// Clear the running app (and the per-app config store binding), closing any
// file still tracked for it (app_files_close_all).  Handles still in the
// app_res list are only reported and dropped (this module cannot free them):
// the runner must have called app_res_release_all first.
void app_identity_end(void);

// The running app, or NULL between apps.
const app_identity_t *app_identity_current(void);

// Did the running app declare requirement `name` (e.g. "sysconfig")?
// False when no app is running.
bool app_identity_has_requirement(const char *name);

// ── Per-app open files ───────────────────────────────────────────────────────
// Every file an app opens through the API (sdcard_fopen handles) is tracked
// against the running app so the runner can close what the app leaked:
// FatFS allows only FF_FS_LOCK (16) open files, so leaked handles would
// break f_open for every later app until a reboot.
//
// Ownership: a handle is closed exactly once, by whoever untracks it.  An
// app-side close calls app_files_untrack() and closes the file only if that
// returns true; app_files_close_all() closes whatever is still tracked.
// Call app_files_close_all() only once nothing app-side can still reach the
// handles (after lua_close, after a native app returned): a freed FIL's
// address can be handed out again by the next sdcard_fopen.

// Track f for the running app.  False (f not tracked) when no app is running
// or the list is full; the caller must then close f and fail the open.
bool app_files_track(void *f);

// Stop tracking f.  True if it was tracked (the caller now owns the close);
// false if it was not (already closed by app_files_close_all, or never
// tracked).
bool app_files_untrack(void *f);

// Close every file still tracked for the running app.  Returns the count.
int app_files_close_all(void);

// Files currently tracked for the running app (0 when none is running).
int app_files_open_count(void);

// ── Per-app handles (native apps) ────────────────────────────────────────────
// Every other object a native app gets through the API (images, players,
// terminals, crypto contexts, qmiAlloc blocks...) is tracked here so the
// loader can free what the app leaked.  Lua apps do not use this: their
// objects are userdata freed by lua_close.
//
// Kinds are the runner's (native_loader.h); this module only stores them.
// Ownership is the same as for files: whoever untracks a handle frees it.
// An app-side free calls app_res_untrack() and frees only if that returns
// true, so a double free or a stray pointer from the app is a no-op.
// The list grows in PSRAM as needed (no fixed cap).

// Track h (of `kind`) for the running app.  False (not tracked) when no app
// is running, h is NULL, or PSRAM is exhausted: the caller must then free h
// and fail the call.
bool app_res_track(int kind, void *h);

// Stop tracking h.  True if it was tracked as `kind` (the caller now frees
// it); false otherwise (never tracked, already freed, or another kind).
bool app_res_untrack(int kind, void *h);

// Untrack every handle, newest first, calling release(kind, h) for each.
// The list is detached before the first call, so release never sees a
// handle twice and may itself call app_res_* safely.  Returns the count.
int app_res_release_all(void (*release)(int kind, void *h));

// Handles currently tracked (0 when no app is running).
int app_res_count(void);

// ── Per-app config store (the native API's appconfig->load) ──────────────────
// Bind the per-app store to `app_id` only if it names the running app
// (case-insensitive: the launcher folds ids to lower case); another app's
// id is refused (false, store left unbound).  With no app running (OS use)
// any id loads.  Lua always binds its own id (lua_bridge_appconfig.c).
bool app_config_load_own(const char *app_id);
