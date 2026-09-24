// Host unit tests for the per-app open-file list in src/os/app_identity.c
// (app_files_track / untrack / close_all), over the in-memory SD fake.
// ASan (the unit build's default) turns a double close into a report: the
// fake's sdcard_fclose frees the handle.
#include "check.h"
#include "app_identity.h"
#include "fakes/sdcard_fake.h"
#include "../../src/drivers/sdcard.h"

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

int main(void) {
  sdfake_reset();
  test_no_app();
  test_track_untrack();
  test_capacity();
  return check_report("test_app_files");
}
