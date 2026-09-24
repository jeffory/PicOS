// Host unit tests for src/os/zip_name.c (ZIP entry-name validation, the
// traversal guard in front of every zip extraction).
#include "check.h"
#include "zip_name.h"

#include <string.h>

typedef struct {
  const char *name;
  bool ok;
} case_t;

static const case_t k_cases[] = {
    {"main.lua", true},
    {"assets/img/a.png", true},
    {"dir/", true},          // directory entry
    {"a/b/c/", true},
    {"foo..bar", true},      // only exact '..' components are hazards
    {"..foo/x", true},
    {".hidden", true},
    {"a/.b", true},
    {"sp ace/ü.txt", true},  // UTF-8 bytes are >= 0x80: allowed
    {NULL, false},
    {"", false},
    {"/etc/passwd", false},  // absolute
    {"../evil", false},
    {"a/../../evil", false},
    {"a/..", false},
    {"..", false},
    {".", false},
    {"./a", false},
    {"a/./b", false},
    {"a//b", false},         // empty component
    {"//a", false},
    {"a\\b", false},         // backslash separator
    {"..\\evil", false},
    {"C:evil", false},       // drive prefix / ADS
    {"a:b", false},
    {"a\nb", false},         // control bytes
    {"a\tb", false},
    {"a\x7f", false},
    {"a\x01", false},
};

static void test_table(void) {
  for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
    bool got = zip_entry_name_valid(k_cases[i].name);
    if (got != k_cases[i].ok)
      printf("  '%s' -> %d, want %d\n", k_cases[i].name ? k_cases[i].name : "(null)",
             got, k_cases[i].ok);
    CHECK(got == k_cases[i].ok);
  }
}

static void test_length(void) {
  char n[ZIP_MAX_NAME + 2];
  memset(n, 'a', sizeof(n));
  n[ZIP_MAX_NAME] = '\0';
  CHECK(zip_entry_name_valid(n));       // exactly 255
  n[ZIP_MAX_NAME] = 'a';
  n[ZIP_MAX_NAME + 1] = '\0';
  CHECK(!zip_entry_name_valid(n));      // 256
}

int main(void) {
  test_table();
  test_length();
  return check_report("test_zip_name");
}
