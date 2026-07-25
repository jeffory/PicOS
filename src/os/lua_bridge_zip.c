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

// ── Module registration ──────────────────────────────────────────────────────

static const luaL_Reg zip_funcs[] = {
    {"extract", l_zip_extract},
    {"list",    l_zip_list},
    {NULL, NULL}
};

void lua_bridge_zip_init(lua_State *L) {
    // Assumes the `picocalc` table is on top of the stack
    lua_newtable(L);
    luaL_setfuncs(L, zip_funcs, 0);
    lua_setfield(L, -2, "zip");
}
