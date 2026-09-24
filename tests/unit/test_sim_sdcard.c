// Host unit tests for the simulator's SD-root containment
// (simulator/hal/hal_sdcard.c hal_sdcard_resolve).
//
// The E2E harness used to reach this through game.save("../../x"); since
// game.save validates names at the bridge no unprivileged Lua path produces a
// "..", so the resolver is pinned here instead.
#include "check.h"
#include "hal_sdcard.h"

#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

extern char g_base_path[512];

static char s_err[256];

void sim_log_err(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_err, sizeof(s_err), fmt, ap);
  va_end(ap);
}

static bool resolve(const char *path, const char *want) {
  char out[1024];
  s_err[0] = '\0';
  bool ok = hal_sdcard_resolve(path, out, sizeof(out));
  if (want == NULL)
    return !ok;
  if (!ok || strcmp(out, want) != 0) {
    printf("  '%s' -> %s '%s', want '%s'\n", path, ok ? "ok" : "refused",
           ok ? out : "", want);
    return false;
  }
  return true;
}

static void test_resolve(void) {
  strcpy(g_base_path, "/sd");
  CHECK(resolve("/saves/x.json", "/sd/saves/x.json"));
  CHECK(resolve("saves/x.json", "/sd/saves/x.json"));
  CHECK(resolve("/./a//b/", "/sd/a/b"));
  CHECK(resolve("/saves/../saves/inside.json", "/sd/saves/inside.json"));
  CHECK(resolve("/a/b/../../c", "/sd/c"));
  CHECK(resolve("/", "/sd"));

  CHECK(resolve("/saves/../../escape.json", NULL));
  CHECK(strstr(s_err, "[SIM] SD escape: /saves/../../escape.json") != NULL);
  CHECK(resolve("/..", NULL));
  CHECK(resolve("../x", NULL));
  CHECK(resolve("/a/../../x", NULL));
}

int main(void) {
  test_resolve();
  return check_report("test_sim_sdcard");
}
