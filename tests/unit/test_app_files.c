// Host unit tests for the per-app open-file list in src/os/app_identity.c
// (app_files_track / untrack / close_all), over the in-memory SD fake; the
// native handle registry (app_res_*) and the own-id appconfig gate.
// ASan (the unit build's default) turns a double close into a report: the
// fake's sdcard_fclose frees the handle.
#include "check.h"
#include "app_identity.h"
#include "fakes/sdcard_fake.h"
#include "../../src/drivers/sdcard.h"
#include "appconfig.h"

#include <string.h>

static app_entry_t make_app(const char *id) {
  app_entry_t a;
  memset(&a, 0, sizeof(a));
  snprintf(a.id, sizeof(a.id), "%s", id);
  snprintf(a.name, sizeof(a.name), "%s", id);
  snprintf(a.path, sizeof(a.path), "/apps/x");
  a.type = APP_TYPE_LUA;
  return a;
}

static sdfile_t open_new(const char *path) {
  return sdcard_fopen(path, "w");
}

static void test_no_app(void) {
  app_identity_end();
  sdfile_t f = open_new("/data/a.txt");
  CHECK(f != NULL);
  CHECK(!app_files_track(f));  // nothing running: refused
  CHECK(!app_files_untrack(f));
  CHECK_EQ_INT(app_files_close_all(), 0);
  CHECK_EQ_INT(app_files_open_count(), 0);
  sdcard_fclose(f);
}

static void test_track_untrack(void) {
  app_entry_t app = make_app("com.test.files");
  CHECK(app_identity_begin(&app));
  sdfile_t a = open_new("/data/a.txt"), b = open_new("/data/b.txt");
  CHECK(app_files_track(a));
  CHECK(app_files_track(b));
  CHECK_EQ_INT(app_files_open_count(), 2);
  CHECK(!app_files_track(NULL));
  // The app closes a: it owns the close only on the first untrack.
  CHECK(app_files_untrack(a));
  CHECK(!app_files_untrack(a));
  sdcard_fclose(a);
  CHECK_EQ_INT(app_files_open_count(), 1);
  // b is left open: the sweep closes it (ASan: exactly once).
  CHECK_EQ_INT(app_files_close_all(), 1);
  CHECK(!app_files_untrack(b));  // a late app-side close does nothing
  CHECK_EQ_INT(app_files_close_all(), 0);
  app_identity_end();
}

static void test_capacity(void) {
  app_entry_t app = make_app("com.test.cap");
  CHECK(app_identity_begin(&app));
  // One path, many handles (the fake's file table is small).
  for (int i = 0; i < APP_FILES_MAX; i++)
    CHECK(app_files_track(open_new("/data/many")));
  sdfile_t extra = open_new("/data/many");
  CHECK(!app_files_track(extra));  // full: the caller closes and fails
  sdcard_fclose(extra);
  CHECK_EQ_INT(app_files_open_count(), APP_FILES_MAX);
  // Ending the app (or starting the next) closes every leftover.
  app_entry_t next = make_app("com.test.next");
  CHECK(app_identity_begin(&next));
  CHECK_EQ_INT(app_files_open_count(), 0);
  app_identity_end();
}

// ── app_res_* ───────────────────────────────────────────────────────────────

static int s_released[64];
static void *s_released_h[64];
static int s_n_released;

static void record_release(int kind, void *h) {
  s_released[s_n_released] = kind;
  s_released_h[s_n_released++] = h;
}

static void test_res_no_app(void) {
  app_identity_end();
  int x;
  CHECK(!app_res_track(1, &x));
  CHECK(!app_res_untrack(1, &x));
  CHECK_EQ_INT(app_res_count(), 0);
  CHECK_EQ_INT(app_res_release_all(record_release), 0);
}

static void test_res_track_release(void) {
  app_entry_t app = make_app("com.test.res");
  app.type = APP_TYPE_NATIVE;
  CHECK(app_identity_begin(&app));
  // More than the initial capacity: the list grows.
  static int objs[40];
  for (int i = 0; i < 40; i++)
    CHECK(app_res_track(i % 3, &objs[i]));
  CHECK(!app_res_track(0, NULL));
  CHECK_EQ_INT(app_res_count(), 40);
  // The app frees one: only the first untrack, and only as its own kind.
  CHECK(!app_res_untrack(2, &objs[0]));  // objs[0] is kind 0
  CHECK(app_res_untrack(0, &objs[0]));
  CHECK(!app_res_untrack(0, &objs[0]));  // double free: no-op
  int stray;
  CHECK(!app_res_untrack(0, &stray));    // never tracked
  CHECK_EQ_INT(app_res_count(), 39);
  // The sweep releases the other 39 exactly once each, kinds intact.
  s_n_released = 0;
  CHECK_EQ_INT(app_res_release_all(record_release), 39);
  CHECK_EQ_INT(s_n_released, 39);
  for (int i = 0; i < s_n_released; i++) {
    int idx = (int)((int *)s_released_h[i] - objs);
    CHECK(idx >= 1 && idx < 40);
    CHECK_EQ_INT(s_released[i], idx % 3);
    for (int j = 0; j < i; j++)
      CHECK(s_released_h[j] != s_released_h[i]);
  }
  CHECK_EQ_INT(app_res_count(), 0);
  CHECK(!app_res_untrack(1, &objs[1]));  // late app-side free: no-op
  CHECK_EQ_INT(app_res_release_all(record_release), 0);
  // Still usable after a sweep; app_identity_end drops (and frees) the list.
  CHECK(app_res_track(5, &objs[5]));
  app_identity_end();
  CHECK_EQ_INT(app_res_count(), 0);
}

// ── app_config_load_own ─────────────────────────────────────────────────────

static void test_config_own_id(void) {
  app_entry_t app = make_app("com.test.cfg");
  CHECK(app_identity_begin(&app));
  CHECK(appconfig_get_app_id() == NULL);  // unbound at start
  CHECK(!app_config_load_own("com.test.other"));
  CHECK(appconfig_get_app_id() == NULL);  // refused: still unbound
  CHECK(!app_config_load_own(NULL));
  app_config_load_own("COM.Test.CFG");     // no file yet: false, but bound
  CHECK(appconfig_get_app_id() != NULL);
  CHECK(strcmp(appconfig_get_app_id(), "com.test.cfg") == 0);
  CHECK(!app_config_load_own("com.test.other"));
  CHECK(strcmp(appconfig_get_app_id(), "com.test.cfg") == 0);  // unchanged
  app_identity_end();
  CHECK(appconfig_get_app_id() == NULL);
}

int main(void) {
  sdfake_reset();
  test_no_app();
  test_track_untrack();
  test_capacity();
  test_res_no_app();
  test_res_track_release();
  test_config_own_id();
  return check_report("test_app_files");
}
