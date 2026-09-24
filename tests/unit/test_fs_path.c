// Host unit tests for src/os/fs_path.c (the fs sandbox predicate).
// Cases follow specs/test-audit-2026-09-24.md §3.1.
#include "check.h"
#include "fs_path.h"

#include <stdbool.h>
#include <stddef.h>

#define APP  "/apps/sandbox_test"
#define DATA "/data/com.test.sandbox"

typedef struct {
  const char *path;
  bool write;
  bool root;
  bool want;
} case_t;

static const case_t k_cases[] = {
    // Baseline (no requirements)
    {"/apps/sandbox_test/main.lua", false, false, true},
    {"/apps/sandbox_test", false, false, true},
    {"/apps/sandbox_test/main.lua", true, false, false},  // bundle is read-only
    {"/data/com.test.sandbox/x", true, false, true},
    {"/data/com.test.sandbox/x", false, false, true},
    {"/data/com.test.sandbox", true, false, true},
    {"/data/com.test.sandbox/sub/dir/f", true, false, true},
    {"/system/config.json", false, false, false},
    {"/system/config.json", true, false, false},
    {"/data/com.other/x", true, false, false},
    {"/data/com.other/x", false, false, false},
    {"/apps/fs_test/main.lua", false, false, false},
    {"apps/sandbox_test/main.lua", false, false, false},  // relative
    {"", false, false, false},
    {"/data/com.test.sandboxEVIL/x", true, false, false},  // prefix confusion
    {"/data/com.test.sandboxEVIL/x", false, false, false},
    {"/apps/sandbox_testEVIL/x", false, false, false},
    {"/data/com.test.sandbox/../com.other/x", true, false, false},  // traversal
    {"/data/com.test.sandbox/a..b", true, false, false},  // any ".." is refused
    {"/apps/sandbox_test/../fs_test/main.lua", false, false, false},
    {"/data", true, false, false},
    {"/", false, false, false},
    // Shared libraries: read-only for everyone
    {"/system/lib/download.lua", false, false, true},
    {"/system/lib/download.lua", true, false, false},
    {"/system/lib/../config.json", false, false, false},
    {"/system/libX/y", false, false, false},
    // root-filesystem
    {"/system/config.json", false, true, true},
    {"/system/config.json", true, true, true},
    {"/data/com.other/x", true, true, true},
    {"/system/lib/download.lua", true, true, true},
    {"/../etc/passwd", false, true, false},  // ".." refused even with root
    {"relative", false, true, false},
};

static void test_table(void) {
  for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
    const case_t *c = &k_cases[i];
    bool got = fs_path_allowed(c->path, c->write, APP, DATA, c->root);
    if (got != c->want)
      printf("  case %zu: '%s' write=%d root=%d -> %d, want %d\n", i, c->path,
             c->write, c->root, got, c->want);
    CHECK(got == c->want);
  }
}

static void test_missing_identity(void) {
  CHECK(!fs_path_allowed(NULL, false, APP, DATA, true));
  CHECK(!fs_path_allowed("/data/com.test.sandbox/x", true, APP, NULL, false));
  CHECK(!fs_path_allowed("/data/com.test.sandbox/x", true, APP, "", false));
  CHECK(!fs_path_allowed("/apps/sandbox_test/x", false, NULL, DATA, false));
  CHECK(!fs_path_allowed("/apps/sandbox_test/x", false, "", DATA, false));
  // An empty dir must not match everything at a boundary ("" + '/').
  CHECK(!fs_path_allowed("/x", true, "", "", false));
  CHECK(fs_path_allowed("/system/lib/x.lua", false, NULL, NULL, false));
}

int main(void) {
  test_table();
  test_missing_identity();
  return check_report("test_fs_path");
}
