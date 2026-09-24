// Host unit tests for src/os/config.c (/system/config.json, the system-wide
// key/value store), over the in-memory SD fake.  Limits per
// specs/test-audit-2026-09-24.md §3.9: 8 entries, keys 31 chars, values 127.
#include "check.h"
#include "config.h"
#include "fakes/sdcard_fake.h"

#include <string.h>

#define PATH "/system/config.json"

static void reset(void) {
  sdfake_reset();
  config_load();  // missing file: empty store
}

static void test_set_get_delete(void) {
  reset();
  CHECK(config_get("a") == NULL);
  config_set("a", "1");
  CHECK_STR(config_get("a"), "1");
  config_set("a", "2");  // overwrite
  CHECK_STR(config_get("a"), "2");
  config_set("a", "");   // empty value deletes
  CHECK(config_get("a") == NULL);
  config_set("b", "x");
  config_set("b", NULL);  // NULL deletes
  CHECK(config_get("b") == NULL);
  config_set("", "ignored");
  config_set(NULL, "ignored");
  CHECK(config_get("") == NULL);
}

static void test_entry_limit(void) {
  reset();
  char k[8];
  for (int i = 0; i < CONFIG_MAX_ENTRIES + 1; i++) {
    snprintf(k, sizeof(k), "k%d", i);
    config_set(k, "v");
  }
  CHECK_STR(config_get("k0"), "v");
  CHECK_STR(config_get("k7"), "v");
  CHECK(config_get("k8") == NULL);  // 9th silently dropped
  // Deleting frees a slot.
  config_set("k0", NULL);
  config_set("k8", "v");
  CHECK_STR(config_get("k8"), "v");
}

static void test_length_limits(void) {
  reset();
  char val[300];
  memset(val, 'v', sizeof(val) - 1);
  val[sizeof(val) - 1] = '\0';
  config_set("long", val);
  CHECK_EQ_INT(strlen(config_get("long")), CONFIG_VAL_MAX - 1);

  char key[64];
  memset(key, 'k', sizeof(key) - 1);
  key[sizeof(key) - 1] = '\0';
  config_set(key, "x");
  // The key is stored truncated to 31 chars, so the full key no longer
  // finds it (documented limit), but the truncated one does.
  CHECK(config_get(key) == NULL);
  key[CONFIG_KEY_MAX - 1] = '\0';
  CHECK_STR(config_get(key), "x");
}

static void test_round_trip_special_chars(void) {
  reset();
  config_set("quote", "say \"hi\"");
  config_set("slash", "a\\b\\");
  config_set("nl", "line1\nline2\ttab");
  config_set("json", "}{,:\"");
  config_set("wifi_pass", "p@ss w0rd!");
  CHECK(config_save());
  const char *file = sdfake_get(PATH, NULL);
  CHECK(file != NULL);

  config_set("quote", NULL);  // drop in-memory state
  CHECK(config_load());
  CHECK_STR(config_get("quote"), "say \"hi\"");
  CHECK_STR(config_get("slash"), "a\\b\\");
  CHECK_STR(config_get("nl"), "line1\nline2\ttab");
  CHECK_STR(config_get("json"), "}{,:\"");
  CHECK_STR(config_get("wifi_pass"), "p@ss w0rd!");
}

static void test_round_trip_full_store(void) {
  reset();
  char k[8], v[CONFIG_VAL_MAX];
  memset(v, '"', sizeof(v) - 1);  // worst case: every char escaped
  v[sizeof(v) - 1] = '\0';
  for (int i = 0; i < CONFIG_MAX_ENTRIES; i++) {
    snprintf(k, sizeof(k), "k%d", i);
    config_set(k, v);
  }
  CHECK(config_save());
  CHECK(config_load());
  for (int i = 0; i < CONFIG_MAX_ENTRIES; i++) {
    snprintf(k, sizeof(k), "k%d", i);
    CHECK_STR(config_get(k), v);
  }
}

// A key containing '"' or '\' is escaped by config_save, so config_load
// must unescape keys as well as values.
static void test_key_escapes_round_trip(void) {
  reset();
  config_set("a\"b", "1");
  config_set("c\\d", "2");
  config_set("e", "3");
  CHECK(config_save());
  CHECK(config_load());
  CHECK_STR(config_get("a\"b"), "1");
  CHECK_STR(config_get("c\\d"), "2");
  CHECK_STR(config_get("e"), "3");
}

// A hand-edited file with an over-long value or key must not desynchronise
// the parser: the following entries still load.
static void test_overlong_file_entries(void) {
  reset();
  char file[1024];
  char big[300];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  snprintf(file, sizeof(file), "{\"long\":\"%s\",\"after\":\"ok\"}", big);
  sdfake_put(PATH, file, strlen(file));
  CHECK(config_load());
  CHECK_EQ_INT(strlen(config_get("long") ? config_get("long") : ""), CONFIG_VAL_MAX - 1);
  CHECK_STR(config_get("after"), "ok");

  big[60] = '\0';
  snprintf(file, sizeof(file), "{\"%s\":\"v\",\"after\":\"ok\"}", big);
  sdfake_put(PATH, file, strlen(file));
  CHECK(config_load());
  CHECK_STR(config_get("after"), "ok");
}

static void test_load_tolerates(void) {
  reset();
  // Whitespace, a non-string value (skipped), empty key (skipped).
  const char *f = "{ \"a\" : \"1\", \"n\": 42, \"\": \"z\", \"b\":\"2\" }";
  sdfake_put(PATH, f, strlen(f));
  CHECK(config_load());
  CHECK_STR(config_get("a"), "1");
  CHECK_STR(config_get("b"), "2");
  CHECK(config_get("n") == NULL);
  // Empty / truncated files.
  sdfake_put(PATH, "", 0);
  CHECK(config_load());
  CHECK(config_get("a") == NULL);
  sdfake_put(PATH, "{\"a\":\"unterminated", 18);
  CHECK(config_load());
  CHECK_STR(config_get("a"), "unterminated");
  sdfake_put(PATH, "{\"a", 3);
  CHECK(config_load());
  CHECK(config_get("a") == NULL);
  // More than 8 entries in the file: the first 8 load.
  const char *nine = "{\"1\":\"a\",\"2\":\"a\",\"3\":\"a\",\"4\":\"a\",\"5\":\"a\","
                     "\"6\":\"a\",\"7\":\"a\",\"8\":\"a\",\"9\":\"a\"}";
  sdfake_put(PATH, nine, strlen(nine));
  CHECK(config_load());
  CHECK_STR(config_get("8"), "a");
  CHECK(config_get("9") == NULL);
  // Missing file.
  sdfake_reset();
  CHECK(!config_load());
}

static void test_save_failures(void) {
  reset();
  config_set("a", "1");
  sdfake_limit_writes(3);
  CHECK(!config_save());  // truncated write reported
  sdfake_limit_writes(-1);
  CHECK(config_save());
  size_t n;
  CHECK_STR(sdfake_get(PATH, &n), "{\"a\":\"1\"}");
}

static void test_brightness_parse(void) {
  CHECK_EQ_INT(config_parse_brightness(NULL), 128);
  CHECK_EQ_INT(config_parse_brightness(""), 128);
  CHECK_EQ_INT(config_parse_brightness("0"), 16);
  CHECK_EQ_INT(config_parse_brightness("200"), 200);
  CHECK_EQ_INT(config_parse_brightness("999"), 255);
  CHECK_EQ_INT(config_parse_brightness("junk"), 16);
}

int main(void) {
  test_set_get_delete();
  test_entry_limit();
  test_length_limits();
  test_round_trip_special_chars();
  test_round_trip_full_store();
  test_key_escapes_round_trip();
  test_overlong_file_entries();
  test_load_tolerates();
  test_save_failures();
  test_brightness_parse();
  sdfake_reset();
  return check_report("test_config");
}
