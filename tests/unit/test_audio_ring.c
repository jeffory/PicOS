// Host unit tests for src/drivers/audio_ring.h: the PCM stream ring between
// the producers (audio_push_samples) and the DMA refill ISR's mixer.
#include "check.h"
#include "audio_ring.h"

#include <stdlib.h>
#include <string.h>

#define OUT 44100u

static audio_ring_t *new_ring(void) {
  audio_ring_t *r = calloc(1, sizeof(*r));
  return r;
}

static void test_push_convert_pop(void) {
  audio_ring_t *r = new_ring();
  int16_t s[] = {-32768, 32767, 0, -1, 256, -256, 0x7f00, -0x7f00};
  CHECK_EQ_INT(audio_ring_push(r, s, 4), 4);
  CHECK_EQ_U32(audio_ring_used(r), 4);
  CHECK_EQ_U32(audio_ring_space(r), AUDIO_RING_SIZE - 4);
  // int16 -> uint8 -> centered int16: the low byte is lost.
  int32_t l, rr;
  CHECK(audio_ring_pop(r, OUT, OUT, &l, &rr));
  CHECK_EQ_INT(l, -32768);
  CHECK_EQ_INT(rr, 127 << 8);  // 32767 -> 255 -> 32512
  CHECK(audio_ring_pop(r, OUT, OUT, &l, &rr));
  CHECK_EQ_INT(l, 0);
  CHECK_EQ_INT(rr, -256);  // -1 -> 127 -> -256
  CHECK(audio_ring_pop(r, OUT, OUT, &l, &rr));
  CHECK_EQ_INT(l, 256);
  CHECK_EQ_INT(rr, -256);
  CHECK(audio_ring_pop(r, OUT, OUT, &l, &rr));
  CHECK_EQ_INT(l, 0x7f00);
  CHECK_EQ_INT(rr, -0x7f00);
  CHECK_EQ_U32(audio_ring_used(r), 0);
  free(r);
}

static void test_underrun(void) {
  audio_ring_t *r = new_ring();
  int32_t l = 99, rr = 99;
  CHECK(!audio_ring_pop(r, OUT, OUT, &l, &rr));
  CHECK_EQ_INT(l, 0);
  CHECK_EQ_INT(rr, 0);
  CHECK_EQ_U32(r->read, 0);
  CHECK_EQ_U32(r->phase, 0);  // an underrun does not advance the resampler
  free(r);
}

static void test_full_drops(void) {
  audio_ring_t *r = new_ring();
  int16_t *buf = calloc(2 * (AUDIO_RING_SIZE + 100), sizeof(int16_t));
  for (unsigned i = 0; i < AUDIO_RING_SIZE + 100; i++)
    buf[2 * i] = buf[2 * i + 1] = (int16_t)((i & 0xff) << 8) - 32768;
  CHECK_EQ_INT(audio_ring_push(r, buf, AUDIO_RING_SIZE + 100), AUDIO_RING_SIZE);
  CHECK_EQ_U32(audio_ring_space(r), 0);
  CHECK_EQ_INT(audio_ring_push(r, buf, 1), 0);  // full: dropped
  // The oldest frames survive; the overflow was dropped, not overwritten.
  int32_t l, rr;
  CHECK(audio_ring_pop(r, OUT, OUT, &l, &rr));
  CHECK_EQ_INT(l, -32768);  // frame 0
  CHECK_EQ_U32(audio_ring_space(r), 1);
  free(buf);
  free(r);
}

// Free-running indices: correct across the uint32 wrap.
static void test_index_wrap(void) {
  audio_ring_t *r = new_ring();
  r->read = r->write = 0xFFFFFFF0u;
  int16_t s[64];
  for (int i = 0; i < 32; i++) {
    s[2 * i] = (int16_t)(i << 8);
    s[2 * i + 1] = (int16_t)-(i << 8);
  }
  CHECK_EQ_INT(audio_ring_push(r, s, 32), 32);
  CHECK_EQ_U32(r->write, 0x10u);  // wrapped
  CHECK_EQ_U32(audio_ring_used(r), 32);
  CHECK_EQ_U32(audio_ring_space(r), AUDIO_RING_SIZE - 32);
  for (int i = 0; i < 32; i++) {
    int32_t l, rr;
    CHECK(audio_ring_pop(r, OUT, OUT, &l, &rr));
    CHECK_EQ_INT(l, i << 8);
  }
  CHECK_EQ_U32(audio_ring_used(r), 0);
  // A corrupted state (read ahead of write) reports no space, never huge.
  r->read = r->write + 1;
  CHECK_EQ_U32(audio_ring_space(r), 0);
  free(r);
}

// Nearest-neighbour rate conversion: each content frame is emitted
// out/content times.
static void check_rate(uint32_t content, const int *want_seq, int n) {
  audio_ring_t *r = new_ring();
  int16_t s[16];
  for (int i = 0; i < 8; i++)
    s[2 * i] = s[2 * i + 1] = (int16_t)((i + 1) << 8);
  audio_ring_push(r, s, 8);
  for (int k = 0; k < n; k++) {
    int32_t l, rr;
    bool ok = audio_ring_pop(r, content, OUT, &l, &rr);
    CHECK(ok);
    if (l >> 8 != want_seq[k])
      printf("  rate %u: output %d is frame %d, want %d\n", (unsigned)content,
             k, (int)(l >> 8), want_seq[k]);
    CHECK_EQ_INT(l >> 8, want_seq[k]);
  }
  free(r);
}

static void test_rates(void) {
  static const int same[] = {1, 2, 3, 4, 5};
  check_rate(44100, same, 5);
  static const int quarter[] = {1, 1, 1, 1, 2, 2, 2, 2, 3};
  check_rate(11025, quarter, 9);
  static const int half[] = {1, 1, 2, 2, 3, 3};
  check_rate(22050, half, 6);
  static const int dbl[] = {1, 3, 5, 7};  // 88200: every other frame
  check_rate(88200, dbl, 4);

  // The accumulator never runs read past write.
  audio_ring_t *r = new_ring();
  int16_t one[2] = {0x100, 0x100};
  audio_ring_push(r, one, 1);
  int32_t l, rr;
  CHECK(audio_ring_pop(r, 176400, OUT, &l, &rr));  // wants to skip 4
  CHECK_EQ_U32(r->read, 1);
  CHECK_EQ_U32(r->write, 1);
  CHECK(!audio_ring_pop(r, 176400, OUT, &l, &rr));
  free(r);
}

static void test_clear(void) {
  audio_ring_t *r = new_ring();
  int16_t s[4] = {1, 2, 3, 4};
  audio_ring_push(r, s, 2);
  audio_ring_clear(r);
  CHECK_EQ_U32(audio_ring_used(r), 0);
  CHECK_EQ_U32(audio_ring_space(r), AUDIO_RING_SIZE);
  free(r);
}

int main(void) {
  test_push_convert_pop();
  test_underrun();
  test_full_drops();
  test_index_wrap();
  test_rates();
  test_clear();
  return check_report("test_audio_ring");
}
