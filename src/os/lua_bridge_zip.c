// lua_bridge_zip.c — picocalc.zip.extract() and picocalc.zip.list()
// Uses miniz for ZIP decompression. Extracts archives to SD card directories.

#include "lua_bridge_zip.h"
#include "lauxlib.h"
#include "sdcard.h"
#include "umm_malloc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
// Redirect miniz allocations to PSRAM (umm_malloc), not tiny SRAM heap
#define MZ_MALLOC(x)     umm_malloc(x)
#define MZ_FREE(x)       umm_free(x)
#define MZ_REALLOC(p, x) umm_realloc(p, x)
#include "miniz.h"

// Forward declaration for sandbox check (defined in lua_bridge_fs.c)
extern bool fs_sandbox_check(lua_State *L, const char *path, bool write);

// Maximum path length for extracted files
#define MAX_EXTRACT_PATH 256

// ── picocalc.zip.list(zip_path) → array of {name, size, compressed_size} ─────
static int l_zip_list(lua_State *L) {
    const char *zip_path = luaL_checkstring(L, 1);

    if (!fs_sandbox_check(L, zip_path, false)) {
        lua_pushnil(L);
        lua_pushstring(L, "permission denied");
        return 2;
    }

    // Read the entire ZIP file into PSRAM (sdcard_read_file uses umm_malloc)
    int zip_len = 0;
    char *zip_data = sdcard_read_file(zip_path, &zip_len);
    if (!zip_data) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to read ZIP file");
        return 2;
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_mem(&zip, zip_data, (size_t)zip_len, 0)) {
        umm_free(zip_data);
        lua_pushnil(L);
        lua_pushstring(L, "invalid ZIP file");
        return 2;
    }

    int num_files = (int)mz_zip_reader_get_num_files(&zip);
    lua_createtable(L, num_files, 0);

    for (int i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, (mz_uint)i, &stat))
            continue;

        // Skip directories
        if (mz_zip_reader_is_file_a_directory(&zip, (mz_uint)i))
            continue;

        lua_createtable(L, 0, 3);
        lua_pushstring(L, stat.m_filename);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)stat.m_uncomp_size);
        lua_setfield(L, -2, "size");
        lua_pushinteger(L, (lua_Integer)stat.m_comp_size);
        lua_setfield(L, -2, "compressed_size");

        lua_rawseti(L, -2, lua_rawlen(L, -2) + 1);
    }

    mz_zip_reader_end(&zip);
    umm_free(zip_data);
    return 1;
}

// Helper: ensure all parent directories exist for a path (like mkdir -p)
static bool ensure_parent_dirs(const char *full_path) {
    char tmp[MAX_EXTRACT_PATH];
    snprintf(tmp, sizeof(tmp), "%s", full_path);

    // Walk forward through path, creating each directory component
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            sdcard_stat_t st;
            if (!sdcard_stat(tmp, &st)) {
                if (!sdcard_mkdir(tmp)) {
                    *p = '/';
                    return false;
                }
            }
            *p = '/';
        }
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

    // Ensure destination directory exists
    sdcard_mkdir(dest_dir);

    // Read the entire ZIP file into PSRAM (sdcard_read_file uses umm_malloc)
    int zip_len = 0;
    char *zip_data = sdcard_read_file(zip_path, &zip_len);
    if (!zip_data) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "failed to read ZIP file");
        return 2;
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_mem(&zip, zip_data, (size_t)zip_len, 0)) {
        umm_free(zip_data);
        lua_pushboolean(L, false);
        lua_pushstring(L, "invalid ZIP file");
        return 2;
    }

    int num_files = (int)mz_zip_reader_get_num_files(&zip);
    // Count actual files (not directories) for progress reporting
    int actual_files = 0;
    for (int i = 0; i < num_files; i++) {
        if (!mz_zip_reader_is_file_a_directory(&zip, (mz_uint)i))
            actual_files++;
    }

    int files_done = 0;
    char full_path[MAX_EXTRACT_PATH];

    for (int i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, (mz_uint)i, &stat))
            continue;

        // Skip directories (they'll be created as needed)
        if (mz_zip_reader_is_file_a_directory(&zip, (mz_uint)i))
            continue;

        // Security: reject paths with ".." to prevent directory traversal
        if (strstr(stat.m_filename, "..")) {
            printf("[ZIP] skipping suspicious path: %s\n", stat.m_filename);
            continue;
        }

        // Build full output path
        size_t dest_len = strlen(dest_dir);
        bool needs_slash = dest_dir[dest_len - 1] != '/';
        snprintf(full_path, sizeof(full_path), "%s%s%s",
                 dest_dir, needs_slash ? "/" : "", stat.m_filename);

        // Ensure parent dirs exist (walks all path components)
        if (!ensure_parent_dirs(full_path)) {
            printf("[ZIP] failed to create parent dirs for: %s\n", full_path);
            continue;
        }

        // Extract file — miniz extracts to a heap buffer, then we write to SD
        // Note: mz_zip_reader_extract_to_heap uses MZ_MALLOC which we redirect
        // to umm_malloc (PSRAM) via compile definitions
        size_t uncomp_size = 0;
        void *file_data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)i,
                                                         &uncomp_size, 0);
        if (!file_data) {
            printf("[ZIP] failed to extract: %s\n", stat.m_filename);
            mz_zip_reader_end(&zip);
            umm_free(zip_data);
            lua_pushboolean(L, false);
            lua_pushfstring(L, "failed to extract: %s", stat.m_filename);
            return 2;
        }

        // Write extracted data to SD card
        sdfile_t f = sdcard_fopen(full_path, "w");
        if (!f) {
            mz_free(file_data);
            mz_zip_reader_end(&zip);
            umm_free(zip_data);
            lua_pushboolean(L, false);
            lua_pushfstring(L, "failed to write: %s", full_path);
            return 2;
        }

        int written = sdcard_fwrite(f, file_data, (int)uncomp_size);
        sdcard_fclose(f);
        mz_free(file_data);

        if (written != (int)uncomp_size) {
            mz_zip_reader_end(&zip);
            umm_free(zip_data);
            lua_pushboolean(L, false);
            lua_pushfstring(L, "incomplete write: %s", full_path);
            return 2;
        }

        files_done++;

        // Call progress callback if provided
        if (has_progress) {
            lua_pushvalue(L, 3);  // push the callback
            lua_pushinteger(L, files_done);
            lua_pushinteger(L, actual_files);
            lua_pcall(L, 2, 0, 0);
        }
    }

    mz_zip_reader_end(&zip);
    umm_free(zip_data);

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
