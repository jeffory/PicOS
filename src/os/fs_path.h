#pragma once

// Filesystem sandbox predicate: may an app touch `path`?
//
// Pure (no Lua, no SD): the Lua bridge's fs_sandbox_check() gathers the
// app's identity and calls this.  Host-tested in tests/unit/test_fs_path.c.
//
// Rules (unchanged from the original fs_sandbox_check):
//   - path must be absolute and contain no ".." anywhere;
//   - reads of /system/lib/... (shared Lua libraries) are always allowed;
//   - root_fs ("root-filesystem" requirement) allows everything else;
//   - data_dir ("/data/<app id>") and everything under it: read + write;
//   - app_dir ("/apps/<dir>") and everything under it: read only.
// A directory matches only at a component boundary, so "/data/com.a" does
// not grant "/data/com.abc".  NULL or empty app_dir / data_dir grant nothing.
// Prefixes compare case-insensitively, as FatFS resolves names.

#include <stdbool.h>

bool fs_path_allowed(const char *path, bool write, const char *app_dir,
                     const char *data_dir, bool root_fs);
