// Host unit tests for src/os/appconfig.c (/data/<APP_ID>/config.json, the
// per-app store behind picocalc.config), over the in-memory SD fake.
// Limits per specs/test-audit-2026-09-24.md §3.9: 4 entries, keys 31 chars,
// values 255.
#include "check.h"
#include "appconfig.h"
#include "fakes/sdcard_fake.h"

#include <string.h>

static void test_limits(void) {
  sdfake_reset();
  appconfig_load("com.test.a");
  char k[8];
  for (int i = 0; i < APPCONFIG_MAX_ENTRIES + 1; i++) {
    snprintf(k, sizeof(k), "k%d", i);
    appconfig_set(k, "v");
  }
  CHECK_STR(appconfig_get("k3", NULL), "v");
  CHECK(appconfig_get("k4", NULL) == NULL);  // 5th dropped
  CHECK_STR(appconfig_get("k4", "dflt"), "dflt");
  CHECK_STR(appconfig_get(NULL, "dflt"), "dflt");

  char val[400];
  memset(val, 'v', sizeof(val) - 1);
  val[sizeof(val) - 1] = '\0';
  appconfig_set("k0", val);
  CHECK_EQ_INT(strlen(appconfig_get("k0", "")), APPCONFIG_VAL_MAX - 1);

  appconfig_set("k1", "");  // delete
  CHECK(appconfig_get("k1", NULL) == NULL);
  appconfig_clear();
  CHECK(appconfig_get("k0", NULL) == NULL);
}

static void test_round_trip(void) {
  sdfake_reset();
  CHECK(!appconfig_load("com.test.rt"));  // first run: no file
  appconfig_set("q", "say \"hi\" \\o/");
  appconfig_set("nl", "a\nb\tc");
  appconfig_set("json", "}{,:");
  appconfig_set("k\"ey", "escaped key");
  CHECK(appconfig_save());
  CHECK(sdfake_dir_exists("/data/com.test.rt"));
  CHECK(sdfake_get("/data/com.test.rt/config.json", NULL) != NULL);

  appconfig_clear();
  CHECK(appconfig_load("com.test.rt"));
  CHECK_STR(appconfig_get("q", NULL), "say \"hi\" \\o/");
  CHECK_STR(appconfig_get("nl", NULL), "a\nb\tc");
  CHECK_STR(appconfig_get("json", NULL), "}{,:");
  CHECK_STR(appconfig_get("k\"ey", NULL), "escaped key");
}

// Loading app B's store replaces app A's (the cross-app bleed in the review
// is the Lua bridge not calling load; see Task 5).
static void test_switch_app(void) {
  sdfake_reset();
  appconfig_load("com.test.a");
  appconfig_set("k", "A");
  CHECK(appconfig_save());
  appconfig_load("com.test.b");
  CHECK(appconfig_get("k", NULL) == NULL);
  CHECK_STR(appconfig_get_app_id(), "com.test.b");
  appconfig_set("k", "B");
  CHECK(appconfig_save());
  CHECK_STR(sdfake_get("/data/com.test.a/config.json", NULL), "{\"k\":\"A\"}");
  CHECK_STR(sdfake_get("/data/com.test.b/config.json", NULL), "{\"k\":\"B\"}");
}

// The file path must hold any id the launcher accepts (app_entry_t.id is 80
// bytes): a long id used to be truncated into "/data/<id>/confi...".
static void test_long_app_id(void) {
  sdfake_reset();
  char id[80];
  memset(id, 'a', sizeof(id) - 1);
  id[sizeof(id) - 1] = '\0';  // 79 chars
  appconfig_load(id);
  CHECK_STR(appconfig_get_app_id(), id);
  appconfig_set("k", "v");
  CHECK(appconfig_save());
  char path[160];
  snprintf(path, sizeof(path), "/data/%s/config.json", id);
  CHECK_STR(sdfake_get(path, NULL), "{\"k\":\"v\"}");
  appconfig_clear();
  CHECK(appconfig_load(id));
  CHECK_STR(appconfig_get("k", NULL), "v");
  CHECK(appconfig_reset());
  CHECK(sdfake_get(path, NULL) == NULL);
}

static void test_overlong_file_entries(void) {
  sdfake_reset();
  char file[1024], big[400];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  snprintf(file, sizeof(file), "{\"long\":\"%s\",\"after\":\"ok\"}", big);
  sdfake_put("/data/com.t/config.json", file, strlen(file));
  CHECK(appconfig_load("com.t"));
  CHECK_STR(appconfig_get("after", NULL), "ok");
}

static void test_no_app_id(void) {
  sdfake_reset();
  CHECK(!appconfig_load(NULL));
  CHECK(!appconfig_load(""));
  // Longer than any launcher id: refused, never truncated into a path.
  char id[100];
  memset(id, 'b', sizeof(id) - 1);
  id[sizeof(id) - 1] = '\0';
  CHECK(!appconfig_load(id));
  CHECK(appconfig_get_app_id() == NULL);
  CHECK(!appconfig_save());
}

static void test_reset(void) {
  sdfake_reset();
  appconfig_load("com.test.r");
  appconfig_set("k", "v");
  CHECK(appconfig_save());
  CHECK(appconfig_reset());
  CHECK(sdfake_get("/data/com.test.r/config.json", NULL) == NULL);
  CHECK(appconfig_get("k", NULL) == NULL);
}

// Between apps the store forgets its app: the next app's first get/set must
// not see (or save over) the previous app's entries, even unsaved ones.
static void test_unbind(void) {
  sdfake_reset();
  appconfig_load("com.test.u");
  appconfig_set("k", "unsaved");
  appconfig_unbind();
  CHECK(appconfig_get_app_id() == NULL);
  CHECK(appconfig_get("k", NULL) == NULL);
  CHECK(!appconfig_save());
  CHECK(sdfake_get("/data/com.test.u/config.json", NULL) == NULL);
}

static void test_save_failure(void) {
  sdfake_reset();
  appconfig_load("com.test.f");
  appconfig_set("k", "v");
  sdfake_limit_writes(2);
  CHECK(!appconfig_save());
  sdfake_limit_writes(-1);
}

static void test_atomic_save(void) {
  const char *P = "/data/com.test.a/config.json";
  sdfake_reset();
  appconfig_load("com.test.a");
  appconfig_set("k", "1");
  CHECK(appconfig_save());
  appconfig_set("k", "2");
  CHECK(appconfig_save());
  CHECK_STR(sdfake_get(P, NULL), "{\"k\":\"2\"}");
  CHECK(sdfake_get("/data/com.test.a/config.json.tmp", NULL) == NULL);
  CHECK(sdfake_get("/data/com.test.a/config.json.bak", NULL) == NULL);
  appconfig_set("k", "3");
  sdfake_fail_rename_after(1);            // moving the new file in fails
  CHECK(!appconfig_save());
  sdfake_fail_rename_after(-1);
  CHECK_STR(sdfake_get(P, NULL), "{\"k\":\"2\"}");
  appconfig_set("k", "4");
  sdfake_limit_writes(2);
  CHECK(!appconfig_save());
  sdfake_limit_writes(-1);
  CHECK_STR(sdfake_get(P, NULL), "{\"k\":\"2\"}");
}

int main(void) {
  // No app loaded yet: save/reset refuse.
  CHECK(appconfig_get_app_id() == NULL);
  CHECK(!appconfig_save());
  CHECK(!appconfig_reset());
  test_limits();
  test_round_trip();
  test_switch_app();
  test_long_app_id();
  test_overlong_file_entries();
  test_no_app_id();
  test_reset();
  test_unbind();
  test_save_failure();
  test_atomic_save();
  sdfake_reset();
  return check_report("test_appconfig");
}
