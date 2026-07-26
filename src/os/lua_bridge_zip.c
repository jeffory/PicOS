// lua_bridge_zip.c — picocalc.zip.extract() and picocalc.zip.list()
// Thin wrappers over the shared hardened ZIP engine (zip_util.c). Sandbox
// checks stay here; all miniz access, streaming and path hardening live in
// the engine.

#include "lua_bridge_zip.h"
#include "lauxlib.h"
#include "zip_util.h"

#include <string.h>

#ifndef PICOS_SIMULATOR
#include "hardware/watchdog.h"
#endif

// Forward declaration for sandbox check (defined in lua_bridge_fs.c)
extern bool fs_sandbox_check(lua_State *L, const char *path, bool write);

// ── picocalc.zip.list(zip_path) → array of {name, size, compressed_size} ─────
static int l_zip_list(lua_State *L) {
    const char *zip_path = luaL_checkstring(L, 1);

    if (!fs_sandbox_check(L, zip_path, false)) {
        lua_pushnil(L);
        lua_pushstring(L, "permission denied");
        return 2;
    }

    zip_reader_t zr;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&zr, zip_path, err)) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }

    int num_files = zip_reader_num_entries(&zr);
    lua_createtable(L, num_files, 0);

    for (int i = 0; i < num_files; i++) {
        zip_entry_info_t info;
        if (!zip_reader_stat_index(&zr, i, &info))
            continue;

        // Skip directories
        if (info.is_dir)
            continue;

        lua_createtable(L, 0, 3);
        lua_pushstring(L, info.name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)info.size);
        lua_setfield(L, -2, "size");
        lua_pushinteger(L, (lua_Integer)info.comp_size);
        lua_setfield(L, -2, "compressed_size");

        lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
    }

    zip_reader_close(&zr);
    return 1;
}

// ── Progress trampoline: pump the watchdog, then fire the Lua callback ───────

typedef struct {
    lua_State *L;
    bool       has_cb;  // Lua callback sitting at stack index 3
} lua_zip_progress_t;

static bool lua_zip_progress(int done, int total, const char *name, void *user) {
    (void)name;
    lua_zip_progress_t *p = (lua_zip_progress_t *)user;
#ifndef PICOS_SIMULATOR
    // Extraction can far outlast the 10s watchdog window; the only other
    // feeder (sys->poll) never runs while we're inside this call.
    watchdog_update();
#endif
    if (p->has_cb) {
        lua_pushvalue(p->L, 3);  // the callback
        lua_pushinteger(p->L, done);
        lua_pushinteger(p->L, total);
        lua_pcall(p->L, 2, 0, 0);  // errors in the callback are ignored
    }
    return true;
}

// ── picocalc.zip.extract(zip_path, dest_dir [, progress_fn]) → ok [, err] ───
static int l_zip_extract(lua_State *L) {
    const char *zip_path = luaL_checkstring(L, 1);
    const char *dest_dir = luaL_checkstring(L, 2);
    bool has_progress = lua_isfunction(L, 3);

    if (!fs_sandbox_check(L, zip_path, false)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "permission denied (source)");
        return 2;
    }
    if (!fs_sandbox_check(L, dest_dir, true)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "permission denied (destination)");
        return 2;
    }

    zip_reader_t zr;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&zr, zip_path, err)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, err);
        return 2;
    }

    lua_zip_progress_t prog = { .L = L, .has_cb = has_progress };
    zip_extract_result_t result;
    bool ok = zip_reader_extract_all(&zr, dest_dir, NULL,
                                     lua_zip_progress, &prog, &result, err);
    zip_reader_close(&zr);

    if (!ok) {
        lua_pushboolean(L, false);
        lua_pushstring(L, err);
        return 2;
    }

    lua_pushboolean(L, true);
    return 1;
}

// ── Read-in-place archive handles: picocalc.zip.open(path) ───────────────────
// Full userdata with a metatable (__gc + __close + closed-handle checks) — a
// deliberate contrast to fs.open's bare lightuserdata, which cannot free
// itself when an app errors out mid-file.

#define ZIP_MT "picocalc.zip.archive"
#define ZIP_LUA_MAX_OPEN 4

typedef struct {
    zip_reader_t zr;    // inline: Lua never moves userdata memory
    bool         open;
} lua_zip_archive_t;

// Open-archive budget for the running app; reset in lua_bridge_zip_init.
static int s_lua_zip_open_count = 0;

static lua_zip_archive_t *check_archive(lua_State *L) {
    lua_zip_archive_t *ar =
        (lua_zip_archive_t *)luaL_checkudata(L, 1, ZIP_MT);
    if (!ar->open)
        luaL_error(L, "archive is closed");
    return ar;
}

static void archive_do_close(lua_zip_archive_t *ar) {
    if (ar->open) {
        zip_reader_close(&ar->zr);
        ar->open = false;
        s_lua_zip_open_count--;
    }
}

// ── zip.open(path) → archive [, err] ─────────────────────────────────────────
static int l_zip_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);

    if (!fs_sandbox_check(L, path, false)) {
        lua_pushnil(L);
        lua_pushstring(L, "permission denied");
        return 2;
    }
    if (s_lua_zip_open_count >= ZIP_LUA_MAX_OPEN) {
        lua_pushnil(L);
        lua_pushstring(L, "too many open archives (max 4)");
        return 2;
    }

    lua_zip_archive_t *ar =
        (lua_zip_archive_t *)lua_newuserdatauv(L, sizeof(*ar), 0);
    ar->open = false;

    char err[ZIP_ERR_MAX];
    if (!zip_reader_open(&ar->zr, path, err)) {
        lua_pop(L, 1);  // the userdata (never opened, __gc is a no-op)
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    ar->open = true;
    s_lua_zip_open_count++;

    luaL_getmetatable(L, ZIP_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// ── ar:list() → array of {name, size, compressed_size} ───────────────────────
static int l_ar_list(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    int n = zip_reader_num_entries(&ar->zr);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        zip_entry_info_t info;
        if (!zip_reader_stat_index(&ar->zr, i, &info) || info.is_dir)
            continue;
        lua_createtable(L, 0, 3);
        lua_pushstring(L, info.name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)info.size);
        lua_setfield(L, -2, "size");
        lua_pushinteger(L, (lua_Integer)info.comp_size);
        lua_setfield(L, -2, "compressed_size");
        lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
    }
    return 1;
}

// ── ar:exists(name) → bool ───────────────────────────────────────────────────
static int l_ar_exists(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *name = luaL_checkstring(L, 2);
    lua_pushboolean(L, zip_reader_locate(&ar->zr, name) >= 0);
    return 1;
}

// ── ar:size(name) → bytes | nil ──────────────────────────────────────────────
static int l_ar_size(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *name = luaL_checkstring(L, 2);
    int idx = zip_reader_locate(&ar->zr, name);
    zip_entry_info_t info;
    if (idx < 0 || !zip_reader_stat_index(&ar->zr, idx, &info)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)info.size);
    return 1;
}

// ── ar:read(name [, max_len]) → string | nil, err ────────────────────────────
static int l_ar_read(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *name = luaL_checkstring(L, 2);
    lua_Integer max_len = luaL_optinteger(L, 3, 0);

    int idx = zip_reader_locate(&ar->zr, name);
    if (idx < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no such entry");
        return 2;
    }

    void *data = NULL;
    size_t len = 0;
    char err[ZIP_ERR_MAX];
    if (!zip_reader_read_to_heap(&ar->zr, idx, &data, &len,
                                 max_len > 0 ? (size_t)max_len : 0, err)) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushlstring(L, (const char *)data, len);
    mz_free(data);
    return 1;
}

// ── ar:extract(name, dest_path) → ok [, err] ─────────────────────────────────
static int l_ar_extract(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *name = luaL_checkstring(L, 2);
    const char *dest = luaL_checkstring(L, 3);

    if (!fs_sandbox_check(L, dest, true)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "permission denied (destination)");
        return 2;
    }
    // Single-entry extraction to a caller-chosen path: the entry name only
    // selects the data, it never becomes a filesystem path, so no name
    // validation is needed here.
    int idx = zip_reader_locate(&ar->zr, name);
    if (idx < 0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "no such entry");
        return 2;
    }
    char err[ZIP_ERR_MAX];
    if (!zip_reader_extract_entry(&ar->zr, idx, dest, err)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

// ── ar:extractAll(dest_dir [, progress_fn]) → ok [, err] ─────────────────────
static int l_ar_extract_all(lua_State *L) {
    lua_zip_archive_t *ar = check_archive(L);
    const char *dest = luaL_checkstring(L, 2);
    bool has_progress = lua_isfunction(L, 3);

    if (!fs_sandbox_check(L, dest, true)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "permission denied (destination)");
        return 2;
    }

    lua_zip_progress_t prog = { .L = L, .has_cb = has_progress };
    zip_extract_result_t result;
    char err[ZIP_ERR_MAX];
    bool ok = zip_reader_extract_all(&ar->zr, dest, NULL,
                                     lua_zip_progress, &prog, &result, err);
    if (!ok) {
        lua_pushboolean(L, false);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

// ── ar:close() ───────────────────────────────────────────────────────────────
static int l_ar_close(lua_State *L) {
    lua_zip_archive_t *ar =
        (lua_zip_archive_t *)luaL_checkudata(L, 1, ZIP_MT);
    archive_do_close(ar);  // double-close is a no-op, not an error
    return 0;
}

// __gc / __close — release the SD file handle when the archive is collected
// or leaves a <close> scope.
static int l_ar_gc(lua_State *L) {
    lua_zip_archive_t *ar =
        (lua_zip_archive_t *)luaL_checkudata(L, 1, ZIP_MT);
    archive_do_close(ar);
    return 0;
}

static const luaL_Reg archive_methods[] = {
    {"list",       l_ar_list},
    {"exists",     l_ar_exists},
    {"size",       l_ar_size},
    {"read",       l_ar_read},
    {"extract",    l_ar_extract},
    {"extractAll", l_ar_extract_all},
    {"close",      l_ar_close},
    {NULL, NULL}
};

// ── Module registration ──────────────────────────────────────────────────────

static const luaL_Reg zip_funcs[] = {
    {"extract", l_zip_extract},
    {"list",    l_zip_list},
    {"open",    l_zip_open},
    {NULL, NULL}
};

void lua_bridge_zip_init(lua_State *L) {
    // Fresh per-app budget. Archives from the previous app were closed by
    // lua_close() running their __gc; this is the safety net.
    s_lua_zip_open_count = 0;

    if (luaL_newmetatable(L, ZIP_MT)) {
        lua_newtable(L);
        luaL_setfuncs(L, archive_methods, 0);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_ar_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, l_ar_gc);
        lua_setfield(L, -2, "__close");
    }
    lua_pop(L, 1);

    // Assumes the `picocalc` table is on top of the stack
    lua_newtable(L);
    luaL_setfuncs(L, zip_funcs, 0);
    lua_setfield(L, -2, "zip");
}
