#include "lua_bridge_internal.h"

// ── picocalc.fs.* ────────────────────────────────────────────────────────────
// Thin wrapper over sdcard_ functions, exposed to Lua

#include "../drivers/sdcard.h"
#include "file_browser.h"
#include "umm_malloc.h"

// ── Filesystem sandbox
// ──────────────────────────────────────────────────────── Apps are allowed to
// access only two trees (unless "root-filesystem" requirement is granted):
//   /apps/<dirname>/  — read-only (their own app bundle)
//   /data/<dirname>/  — read + write (their own data directory)
//
// <dirname> is derived from the APP_DIR global set by launcher.c, e.g.
//   APP_DIR = "/apps/editor"  → dirname = "editor"
//
// Relative paths and any path containing ".." are always rejected.
//
// Apps with the "root-filesystem" requirement can access the entire SD card.

bool fs_sandbox_check(lua_State *L, const char *path, bool write) {
  if (!path || path[0] != '/')
    return false; // require absolute paths
  if (strstr(path, ".."))
    return false; // reject traversal

  // Check for root-filesystem requirement
  lua_getglobal(L, "APP_REQUIREMENTS");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "root_filesystem");
    bool has_root_fs = lua_toboolean(L, -1);
    lua_pop(L, 2); // pop root_filesystem and APP_REQUIREMENTS
    if (has_root_fs) {
      return true; // full filesystem access granted
    }
  } else {
    lua_pop(L, 1); // pop APP_REQUIREMENTS if not a table
  }

  lua_getglobal(L, "APP_DIR");
  const char *app_dir = lua_tostring(L, -1);
  lua_pop(L, 1);
  if (!app_dir)
    return false;

  // Extract the directory name component from "/apps/<dirname>"
  const char *dirname = strrchr(app_dir, '/');
  if (!dirname || dirname[1] == '\0')
    return false;
  dirname++; // skip the '/'

  // /data/<APP_ID> prefix — uses the app's declared identity, not its folder name
  lua_getglobal(L, "APP_ID");
  const char *app_id = lua_tostring(L, -1);
  lua_pop(L, 1);
  if (!app_id)
    return false;
  char data_prefix[128];
  int dp_len = snprintf(data_prefix, sizeof(data_prefix), "/data/%s", app_id);
  bool in_data = (strncmp(path, data_prefix, dp_len) == 0 &&
                  (path[dp_len] == '\0' || path[dp_len] == '/'));

  if (write)
    return in_data;

  // For reads also allow /apps/<dirname> itself and any path beneath it
  char app_prefix[128];
  int ap_len = snprintf(app_prefix, sizeof(app_prefix), "/apps/%s", dirname);
  bool in_app = (strncmp(path, app_prefix, ap_len) == 0 &&
                 (path[ap_len] == '\0' || path[ap_len] == '/'));

  return in_data || in_app;
}

static int l_fs_open(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  const char *mode = luaL_optstring(L, 2, "r");
  bool needs_write = (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL ||
                      strchr(mode, '+') != NULL);
  if (!fs_sandbox_check(L, path, needs_write)) {
    lua_pushnil(L);
    return 1;
  }
  sdfile_t f = sdcard_fopen(path, mode);
  if (!f) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlightuserdata(L, f);
  return 1;
}

static int l_fs_read(lua_State *L) {
  sdfile_t f = lua_touserdata(L, 1);
  int len = (int)luaL_checkinteger(L, 2);
  char *buf = (char *)umm_malloc(len);
  if (!buf) {
    lua_pushnil(L);
    return 1;
  }
  int n = sdcard_fread(f, buf, len);
  if (n <= 0) {
    umm_free(buf);
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, buf, n);
  umm_free(buf);
  return 1;
}

static int l_fs_write(lua_State *L) {
  sdfile_t f = lua_touserdata(L, 1);
  size_t len;
  const char *data = luaL_checklstring(L, 2, &len);
  int n = sdcard_fwrite(f, data, (int)len);
  lua_pushinteger(L, n);
  return 1;
}

static int l_fs_close(lua_State *L) {
  sdfile_t f = lua_touserdata(L, 1);
  sdcard_fclose(f);
  return 0;
}

static int l_fs_exists(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushboolean(L, false);
    return 1;
  }
  lua_pushboolean(L, sdcard_fexists(path));
  return 1;
}

static int l_fs_readFile(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushnil(L);
    return 1;
  }
  int len = 0;
  char *buf = sdcard_read_file(path, &len);
  if (!buf) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, buf, len);
  umm_free(buf);
  return 1;
}

static int l_fs_seek(lua_State *L) {
  sdfile_t f = lua_touserdata(L, 1);
  uint32_t offset = (uint32_t)luaL_checkinteger(L, 2);
  lua_pushboolean(L, sdcard_fseek(f, offset));
  return 1;
}

static int l_fs_tell(lua_State *L) {
  sdfile_t f = lua_touserdata(L, 1);
  lua_pushinteger(L, (lua_Integer)sdcard_ftell(f));
  return 1;
}

static int l_fs_size(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushinteger(L, -1);
    return 1;
  }
  lua_pushinteger(L, sdcard_fsize(path));
  return 1;
}

// Context passed through sdcard_list_dir's void* user pointer
typedef struct {
  lua_State *L;
  int tidx;
  int n;
} listdir_ctx_t;

// Decode FatFS packed date/time fields into separate Lua table entries.
// fdate bits: [15:9]=year-1980, [8:5]=month, [4:0]=day
// ftime bits: [15:11]=hour, [10:5]=min, [4:0]=sec/2
static void push_mtime_fields(lua_State *L, uint16_t fdate, uint16_t ftime) {
  if (fdate == 0) return; // no timestamp stored
  lua_pushinteger(L, (fdate >> 9) + 1980); lua_setfield(L, -2, "year");
  lua_pushinteger(L, (fdate >> 5) & 0xF);  lua_setfield(L, -2, "month");
  lua_pushinteger(L, fdate & 0x1F);         lua_setfield(L, -2, "day");
  lua_pushinteger(L, ftime >> 11);           lua_setfield(L, -2, "hour");
  lua_pushinteger(L, (ftime >> 5) & 0x3F);  lua_setfield(L, -2, "min");
  lua_pushinteger(L, (ftime & 0x1F) * 2);   lua_setfield(L, -2, "sec");
}

static void listdir_cb(const sdcard_entry_t *e, void *user) {
  listdir_ctx_t *ctx = (listdir_ctx_t *)user;
  lua_State *L = ctx->L;
  lua_newtable(L);
  lua_pushstring(L, e->name);
  lua_setfield(L, -2, "name");
  lua_pushboolean(L, e->is_dir);
  lua_setfield(L, -2, "is_dir");
  lua_pushinteger(L, e->size);
  lua_setfield(L, -2, "size");
  push_mtime_fields(L, e->fdate, e->ftime);
  lua_rawseti(L, ctx->tidx, ++ctx->n);
}

// Returns an array of {name, is_dir, size} tables, or an empty table on error.
static int l_fs_listDir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  lua_newtable(L);
  if (!fs_sandbox_check(L, path, false))
    return 1; // return empty table
  listdir_ctx_t ctx = {L, lua_gettop(L), 0};
  sdcard_list_dir(path, listdir_cb, &ctx);
  return 1;
}

static int l_fs_mkdir(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, true)) {
    lua_pushboolean(L, false);
    return 1;
  }
  lua_pushboolean(L, sdcard_mkdir(path));
  return 1;
}

// Convenience: return the path /data/<dirname>/<name>, auto-creating the
// data directory if it does not already exist.
// Usage: local path = picocalc.fs.appPath("save.json")
static int l_fs_appPath(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);

  lua_getglobal(L, "APP_ID");
  const char *app_id = lua_tostring(L, -1);
  lua_pop(L, 1);
  if (!app_id) {
    lua_pushnil(L);
    return 1;
  }

  // Auto-create /data/<APP_ID>/ on first call
  char data_dir[128];
  snprintf(data_dir, sizeof(data_dir), "/data/%s", app_id);
  sdcard_mkdir(data_dir);

  char full_path[192];
  snprintf(full_path, sizeof(full_path), "/data/%s/%s", app_id, name);
  lua_pushstring(L, full_path);
  return 1;
}

// Open a file-browser panel overlay.
// Optional arg: start directory (defaults to the app's /data/<dirname>/ dir).
// Returns the selected file path as a string, or nil if cancelled.
static int l_fs_browse(lua_State *L) {
  const char *start_path;
  static char default_path[128];

  // Always determine the app's data root for use as the browser root boundary.
  lua_getglobal(L, "APP_ID");
  const char *app_id = lua_tostring(L, -1);
  lua_pop(L, 1);

  const char *root_path;
  static char root_buf[128];
  if (app_id) {
    snprintf(root_buf, sizeof(root_buf), "/data/%s", app_id);
    sdcard_mkdir(root_buf);
    root_path = root_buf;
  } else {
    root_path = "/data";
  }

  if (lua_isnoneornil(L, 1)) {
    start_path = root_path;
  } else {
    start_path = luaL_checkstring(L, 1);
  }

  char selected[192];
  if (file_browser_show(start_path, root_path, selected, sizeof(selected))) {
    lua_pushstring(L, selected);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

// ── picocalc.fs.delete(path) → ok [, err] ────────────────────────────────────
static int l_fs_delete(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied");
    return 2;
  }
  if (sdcard_delete(path)) {
    lua_pushboolean(L, true);
    return 1;
  }
  lua_pushboolean(L, false);
  lua_pushstring(L, "delete failed");
  return 2;
}

// ── picocalc.fs.rename(src, dst) → ok [, err] ────────────────────────────────
static int l_fs_rename(lua_State *L) {
  const char *src = luaL_checkstring(L, 1);
  const char *dst = luaL_checkstring(L, 2);
  if (!fs_sandbox_check(L, src, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied (source)");
    return 2;
  }
  if (!fs_sandbox_check(L, dst, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied (destination)");
    return 2;
  }
  if (sdcard_rename(src, dst)) {
    lua_pushboolean(L, true);
    return 1;
  }
  lua_pushboolean(L, false);
  lua_pushstring(L, "rename failed");
  return 2;
}

// ── picocalc.fs.copy(src, dst [, progress_fn]) → ok [, err] ─────────────────
typedef struct { lua_State *L; int fn_ref; } copy_progress_ctx_t;

static void copy_progress_trampoline(uint32_t done, uint32_t total, void *user) {
  copy_progress_ctx_t *ctx = (copy_progress_ctx_t *)user;
  lua_rawgeti(ctx->L, LUA_REGISTRYINDEX, ctx->fn_ref);
  lua_pushinteger(ctx->L, (lua_Integer)done);
  lua_pushinteger(ctx->L, (lua_Integer)total);
  lua_pcall(ctx->L, 2, 0, 0);
}

static int l_fs_copy(lua_State *L) {
  const char *src = luaL_checkstring(L, 1);
  const char *dst = luaL_checkstring(L, 2);
  if (!fs_sandbox_check(L, src, false)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied (source)");
    return 2;
  }
  if (!fs_sandbox_check(L, dst, true)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "permission denied (destination)");
    return 2;
  }

  copy_progress_ctx_t ctx = {NULL, LUA_NOREF};
  if (lua_isfunction(L, 3)) {
    lua_pushvalue(L, 3);
    ctx.L      = L;
    ctx.fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  bool ok = sdcard_copy(src, dst,
                        ctx.fn_ref != LUA_NOREF ? copy_progress_trampoline : NULL,
                        &ctx);
  if (ctx.fn_ref != LUA_NOREF)
    luaL_unref(L, LUA_REGISTRYINDEX, ctx.fn_ref);

  if (ok) {
    lua_pushboolean(L, true);
    return 1;
  }
  lua_pushboolean(L, false);
  lua_pushstring(L, "copy failed");
  return 2;
}

// ── picocalc.fs.stat(path) → {size, is_dir, year, month, day, hour, min, sec} ─
static int l_fs_stat(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!fs_sandbox_check(L, path, false)) {
    lua_pushnil(L);
    lua_pushstring(L, "permission denied");
    return 2;
  }
  sdcard_stat_t st;
  if (!sdcard_stat(path, &st)) {
    lua_pushnil(L);
    lua_pushstring(L, "not found");
    return 2;
  }
  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)st.size); lua_setfield(L, -2, "size");
  lua_pushboolean(L, st.is_dir);            lua_setfield(L, -2, "is_dir");
  push_mtime_fields(L, st.fdate, st.ftime);
  return 1;
}

// ── picocalc.fs.diskInfo() → {free, total}  (values in KB) ──────────────────
static int l_fs_diskInfo(lua_State *L) {
  uint32_t free_kb = 0, total_kb = 0;
  if (!sdcard_disk_info(&free_kb, &total_kb)) {
    lua_pushnil(L);
    lua_pushstring(L, "disk info unavailable");
    return 2;
  }
  lua_newtable(L);
  lua_pushinteger(L, (lua_Integer)free_kb);  lua_setfield(L, -2, "free");
  lua_pushinteger(L, (lua_Integer)total_kb); lua_setfield(L, -2, "total");
  return 1;
}

// ── picocalc.fs.glob(path, pattern) → [{name, is_dir, size, year, …}] ────────
// Pattern supports * (any sequence) and ? (any single char); case-insensitive.
static bool glob_match(const char *pat, const char *str) {
  const char *star = NULL, *ss = str;
  while (*str) {
    char p = (*pat >= 'A' && *pat <= 'Z') ? (*pat + 32) : *pat;
    char s = (*str >= 'A' && *str <= 'Z') ? (*str + 32) : *str;
    if (p == '?' || p == s) {
      pat++; str++;
    } else if (*pat == '*') {
      star = pat++; ss = str;
    } else if (star) {
      pat = star + 1; str = ++ss;
    } else {
      return false;
    }
  }
  while (*pat == '*') pat++;
  return !*pat;
}

typedef struct { lua_State *L; int tidx; int n; const char *pattern; } glob_ctx_t;

static void glob_cb(const sdcard_entry_t *e, void *user) {
  glob_ctx_t *ctx = (glob_ctx_t *)user;
  if (!glob_match(ctx->pattern, e->name)) return;
  lua_State *L = ctx->L;
  lua_newtable(L);
  lua_pushstring(L, e->name);           lua_setfield(L, -2, "name");
  lua_pushboolean(L, e->is_dir);        lua_setfield(L, -2, "is_dir");
  lua_pushinteger(L, (lua_Integer)e->size); lua_setfield(L, -2, "size");
  push_mtime_fields(L, e->fdate, e->ftime);
  lua_rawseti(L, ctx->tidx, ++ctx->n);
}

static int l_fs_glob(lua_State *L) {
  const char *path    = luaL_checkstring(L, 1);
  const char *pattern = luaL_checkstring(L, 2);
  lua_newtable(L);
  if (!fs_sandbox_check(L, path, false)) return 1;
  glob_ctx_t ctx = {L, lua_gettop(L), 0, pattern};
  sdcard_list_dir(path, glob_cb, &ctx);
  return 1;
}

static const luaL_Reg l_fs_lib[] = {
    {"open",     l_fs_open},     {"read",     l_fs_read},
    {"write",    l_fs_write},    {"close",    l_fs_close},
    {"seek",     l_fs_seek},     {"tell",     l_fs_tell},
    {"exists",   l_fs_exists},   {"readFile", l_fs_readFile},
    {"size",     l_fs_size},     {"listDir",  l_fs_listDir},
    {"mkdir",    l_fs_mkdir},    {"appPath",  l_fs_appPath},
    {"browse",   l_fs_browse},
    {"delete",   l_fs_delete},   {"rename",   l_fs_rename},
    {"copy",     l_fs_copy},     {"stat",     l_fs_stat},
    {"diskInfo", l_fs_diskInfo}, {"glob",     l_fs_glob},
    {NULL, NULL}};


void lua_bridge_fs_init(lua_State *L) {
  register_subtable(L, "fs", l_fs_lib);
}
