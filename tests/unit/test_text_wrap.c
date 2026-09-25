// Host unit tests for src/os/text_wrap.c (shared word wrap used by the
// terminal renderer and UI widgets).
#include "check.h"
#include "text_wrap.h"

#include <string.h>

static void test_find_break(void) {
  // Fits: no break.
  CHECK_EQ_INT(text_wrap_find_break("hello", 5, 10), 0);
  CHECK_EQ_INT(text_wrap_find_break("hello", 5, 5), 0);
  // Break after the last space within max_cols.
  const char *t = "hello world again";
  CHECK_EQ_INT(text_wrap_find_break(t, (int)strlen(t), 10), 6);   // "hello "
  CHECK_EQ_INT(text_wrap_find_break(t, (int)strlen(t), 12), 12);  // "hello world "
  // Punctuation breaks too, after the character.
  const char *p = "one,two;three";
  CHECK_EQ_INT(text_wrap_find_break(p, (int)strlen(p), 6), 4);    // "one,"
  const char *d = "well-known fact";
  CHECK_EQ_INT(text_wrap_find_break(d, (int)strlen(d), 8), 5);    // "well-"
  // No break character: force break at max_cols.
  const char *w = "abcdefghijklmnop";
  CHECK_EQ_INT(text_wrap_find_break(w, (int)strlen(w), 5), 5);
  // Only the last 20 columns are searched: a space further back is ignored.
  char longw[64];
  memset(longw, 'x', sizeof(longw));
  longw[2] = ' ';
  CHECK_EQ_INT(text_wrap_find_break(longw, 64, 30), 30);
  longw[12] = ' ';
  CHECK_EQ_INT(text_wrap_find_break(longw, 64, 30), 13);
  // max_cols 1: search stops at column 1.
  CHECK_EQ_INT(text_wrap_find_break("ab", 2, 1), 1);
  CHECK_EQ_INT(text_wrap_find_break(" b", 2, 1), 1);
}

static void test_segments(void) {
  int seg[8];
  const char *t = "hello world again";
  // "hello " | "world " | "again" ("world again" is 11 > 10)
  int n = text_wrap_segments(t, (int)strlen(t), 10, seg, 8);
  CHECK_EQ_INT(n, 3);
  CHECK_EQ_INT(seg[0], 0);
  CHECK_EQ_INT(seg[1], 6);
  CHECK_EQ_INT(seg[2], 12);
  // Wide enough for the tail: "hello world " | "again"
  n = text_wrap_segments(t, (int)strlen(t), 12, seg, 8);
  CHECK_EQ_INT(n, 2);
  CHECK_EQ_INT(seg[1], 12);
  n = text_wrap_segments(t, (int)strlen(t), 7, seg, 8);
  CHECK_EQ_INT(n, 3);
  CHECK_EQ_INT(seg[1], 6);
  CHECK_EQ_INT(seg[2], 12);

  // Empty text and bad widths: one row at offset 0.
  seg[0] = 99;
  CHECK_EQ_INT(text_wrap_segments("", 0, 10, seg, 8), 1);
  CHECK_EQ_INT(seg[0], 0);
  CHECK_EQ_INT(text_wrap_segments("abc", 3, 0, seg, 8), 1);
  CHECK_EQ_INT(text_wrap_segments("abc", 3, -5, seg, 8), 1);
  // NULL segments: count only.
  CHECK_EQ_INT(text_wrap_segments("aaaaaaaaaa", 10, 3, NULL, 8), 4);
  // Capped at max_segments.
  CHECK_EQ_INT(text_wrap_segments("aaaaaaaaaa", 10, 3, seg, 2), 2);
  CHECK_EQ_INT(seg[1], 3);
  // max_segments 0 still reports one row and writes nothing.
  seg[0] = 77;
  CHECK_EQ_INT(text_wrap_segments("aaaaaaaaaa", 10, 3, seg, 0), 1);
  CHECK_EQ_INT(seg[0], 77);
}

// Every row fits in max_cols and the rows cover the text in order.
static void test_invariants(void) {
  const char *txt =
      "The quick brown fox jumps over the lazy dog; pack my box with five "
      "dozen liquor jugs! Sphinx-of-black-quartz,judge-my-vow.";
  int len = (int)strlen(txt);
  for (int cols = 1; cols <= 40; cols++) {
    int seg[256];
    int n = text_wrap_segments(txt, len, cols, seg, 256);
    CHECK(n >= 1);
    CHECK_EQ_INT(seg[0], 0);
    for (int i = 0; i < n; i++) {
      int end = i + 1 < n ? seg[i + 1] : len;
      CHECK(end > seg[i]);
      CHECK(end - seg[i] <= cols);
    }
  }
}

int main(void) {
  test_find_break();
  test_segments();
  test_invariants();
  return check_report("test_text_wrap");
}
