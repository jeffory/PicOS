// Zero-dependency assertions for the host unit tests (tests/unit).
//
//   #include "check.h"
//   static void test_x(void) { CHECK(a == b); CHECK_EQ_U32(x, 3); }
//   int main(void) { test_x(); return check_report("test_x"); }
//
// A failing CHECK prints file:line and keeps going, so one run shows every
// failure; check_report() returns non-zero if any failed (ctest's verdict).
#pragma once

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int s_check_fails = 0;
static int s_check_count = 0;

#define CHECK(cond) do { s_check_count++; if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_check_fails++; } } while (0)

#define CHECK_EQ_U32(got, want) do { s_check_count++; \
  uint32_t _g = (uint32_t)(got), _w = (uint32_t)(want); if (_g != _w) { \
  printf("FAIL %s:%d: %s == 0x%" PRIx32 ", want 0x%" PRIx32 " (%s)\n", \
         __FILE__, __LINE__, #got, _g, _w, #want); s_check_fails++; } } while (0)

#define CHECK_EQ_INT(got, want) do { s_check_count++; \
  long long _g = (long long)(got), _w = (long long)(want); if (_g != _w) { \
  printf("FAIL %s:%d: %s == %lld, want %lld (%s)\n", \
         __FILE__, __LINE__, #got, _g, _w, #want); s_check_fails++; } } while (0)

#define CHECK_STR(got, want) do { s_check_count++; \
  const char *_g = (got), *_w = (want); \
  if (!_g || !_w || strcmp(_g, _w) != 0) { \
  printf("FAIL %s:%d: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #got, \
         _g ? _g : "(null)", _w ? _w : "(null)"); s_check_fails++; } } while (0)

static inline int check_report(const char *suite) {
  if (s_check_fails)
    printf("%s: %d of %d checks FAILED\n", suite, s_check_fails, s_check_count);
  else
    printf("%s: all %d checks passed\n", suite, s_check_count);
  return s_check_fails ? 1 : 0;
}
