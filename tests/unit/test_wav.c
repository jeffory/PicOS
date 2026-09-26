// Host unit tests for src/drivers/wav.c (RIFF/WAVE header parsing shared by
// sound.c, fileplayer.c and the simulator).  The review's Low row: the old
// parsers read only the first 44 bytes and always seeked to 44, so LIST
// chunks and EXTENSIBLE files failed and bit depth was unchecked.
#include "check.h"
#include "wav.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t b[1024];
  size_t n;
} wbuf_t;

static void put(wbuf_t *w, const void *p, size_t n) {
  memcpy(w->b + w->n, p, n);
  w->n += n;
}
static void put16(wbuf_t *w, uint16_t v) { uint8_t b[2] = {v & 0xff, v >> 8}; put(w, b, 2); }
static void put32(wbuf_t *w, uint32_t v) {
  uint8_t b[4] = {v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24};
  put(w, b, 4);
}
static void chunk(wbuf_t *w, const char *id, uint32_t size) { put(w, id, 4); put32(w, size); }

static void riff(wbuf_t *w) {
  w->n = 0;
  put(w, "RIFF", 4);
  put32(w, 0);  // RIFF size: not checked (streamed files often have 0/-1)
  put(w, "WAVE", 4);
}

static void fmt(wbuf_t *w, uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits) {
  chunk(w, "fmt ", 16);
  put16(w, tag);
  put16(w, ch);
  put32(w, rate);
  put32(w, rate * ch * bits / 8);
  put16(w, (uint16_t)(ch * bits / 8));
  put16(w, bits);
}

static void fmt_ext(wbuf_t *w, uint16_t sub, uint16_t ch, uint32_t rate, uint16_t bits) {
  chunk(w, "fmt ", 40);
  put16(w, 0xFFFE);
  put16(w, ch);
  put32(w, rate);
  put32(w, rate * ch * bits / 8);
  put16(w, (uint16_t)(ch * bits / 8));
  put16(w, bits);
  put16(w, 22);    // cbSize
  put16(w, bits);  // valid bits
  put32(w, 3);     // channel mask
  put16(w, sub);   // SubFormat GUID (first 2 bytes = format tag)
  static const uint8_t guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                        0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  put(w, guid_tail, sizeof(guid_tail));
}

static void data(wbuf_t *w, uint32_t size) {
  chunk(w, "data", size);
  for (uint32_t i = 0; i < size && w->n < sizeof(w->b); i++)
    w->b[w->n++] = (uint8_t)i;
}

// Parse from an exact-size heap copy so ASan sees any overread.
static wav_err_t parse(const wbuf_t *w, size_t len, wav_info_t *info) {
  uint8_t *h = malloc(len ? len : 1);
  memcpy(h, w->b, len);
  wav_err_t e = wav_parse(h, len, info);
  free(h);
  return e;
}

static void test_canonical(void) {
  wbuf_t w;
  wav_info_t i;
  riff(&w);
  fmt(&w, 1, 2, 44100, 16);
  data(&w, 64);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_OK);
  CHECK_EQ_U32(i.channels, 2);
  CHECK_EQ_U32(i.bits_per_sample, 16);
  CHECK_EQ_U32(i.sample_rate, 44100);
  CHECK_EQ_U32(i.block_align, 4);
  CHECK_EQ_U32(i.data_offset, 44);
  CHECK_EQ_U32(i.data_size, 64);
  // Only the 44-byte header needs to be in the buffer.
  CHECK_EQ_INT(parse(&w, 44, &i), WAV_OK);
  CHECK_EQ_U32(i.data_size, 64);

  riff(&w);
  fmt(&w, 1, 1, 11025, 8);
  data(&w, 7);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_OK);
  CHECK_EQ_U32(i.block_align, 1);
  CHECK_EQ_U32(i.data_size, 7);
}

static void test_list_chunks(void) {
  wbuf_t w;
  wav_info_t i;
  // LIST/INFO before fmt and between fmt and data (Audacity/ffmpeg files).
  riff(&w);
  chunk(&w, "LIST", 26);
  put(&w, "INFOISFT\x0e\0\0\0Lavf58.29.100\0", 26);
  fmt(&w, 1, 2, 22050, 16);
  chunk(&w, "fact", 4);
  put32(&w, 16);
  data(&w, 64);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_OK);
  CHECK_EQ_U32(i.data_offset, 12 + 34 + 24 + 12 + 8);
  CHECK_EQ_U32(i.sample_rate, 22050);

  // Odd-sized chunk: padded to even.
  riff(&w);
  fmt(&w, 1, 1, 8000, 16);
  chunk(&w, "junk", 3);
  put(&w, "abc\0", 4);  // 3 bytes + pad
  data(&w, 8);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_OK);
  CHECK_EQ_U32(i.data_offset, 12 + 24 + 12 + 8);
}

static void test_extensible(void) {
  wbuf_t w;
  wav_info_t i;
  riff(&w);
  fmt_ext(&w, 1, 2, 48000, 16);
  data(&w, 16);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_OK);
  CHECK_EQ_U32(i.data_offset, 12 + 48 + 8);
  CHECK_EQ_U32(i.sample_rate, 48000);
  riff(&w);
  fmt_ext(&w, 3, 2, 48000, 32);  // IEEE float inside EXTENSIBLE
  data(&w, 16);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_ERR_UNSUPPORTED);
  // EXTENSIBLE tag with a 16-byte fmt: malformed.
  riff(&w);
  fmt(&w, 0xFFFE, 2, 48000, 16);
  data(&w, 16);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_ERR_BAD_FMT);
}

static void test_unsupported(void) {
  wbuf_t w;
  wav_info_t i;
  struct { uint16_t tag, ch; uint32_t rate; uint16_t bits; } bad[] = {
      {1, 2, 44100, 24},  // 24-bit: was accepted and played as noise
      {1, 2, 44100, 32},
      {3, 2, 44100, 32},  // IEEE float
      {2, 1, 44100, 4},   // ADPCM
      {1, 3, 44100, 16},  // 3 channels
      {1, 0, 44100, 16},
      {1, 2, 0, 16},
      {1, 2, 400000, 16},
  };
  for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
    riff(&w);
    fmt(&w, bad[k].tag, bad[k].ch, bad[k].rate, bad[k].bits);
    data(&w, 16);
    CHECK_EQ_INT(parse(&w, w.n, &i), WAV_ERR_UNSUPPORTED);
  }
}

static void test_malformed(void) {
  wbuf_t w;
  wav_info_t i;
  riff(&w);
  CHECK_EQ_INT(parse(&w, 11, &i), WAV_ERR_NOT_WAV);
  CHECK_EQ_INT(parse(&w, 12, &i), WAV_ERR_NO_FMT);
  CHECK_EQ_INT(wav_parse(NULL, 12, &i), WAV_ERR_NOT_WAV);
  w.b[0] = 'X';
  CHECK_EQ_INT(parse(&w, 12, &i), WAV_ERR_NOT_WAV);
  riff(&w);
  memcpy(w.b + 8, "AVI ", 4);
  CHECK_EQ_INT(parse(&w, 12, &i), WAV_ERR_NOT_WAV);

  // data before fmt
  riff(&w);
  data(&w, 8);
  fmt(&w, 1, 2, 44100, 16);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_ERR_NO_FMT);

  // fmt without data
  riff(&w);
  fmt(&w, 1, 2, 44100, 16);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_ERR_NO_DATA);

  // fmt chunk too small / truncated by the buffer
  riff(&w);
  chunk(&w, "fmt ", 14);
  put(&w, "\1\0\2\0\x44\xac\0\0\x10\xb1\2\0\4\0", 14);
  data(&w, 8);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_ERR_BAD_FMT);
  riff(&w);
  fmt(&w, 1, 2, 44100, 16);
  CHECK_EQ_INT(parse(&w, 30, &i), WAV_ERR_BAD_FMT);

  // A chunk whose size runs past the buffer (or wraps): no data found, no
  // overread, no endless loop.
  uint32_t sizes[] = {0xFFFFFFFFu, 0xFFFFFFF7u, 0x80000000u, 1000};
  for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
    riff(&w);
    fmt(&w, 1, 2, 44100, 16);
    chunk(&w, "LIST", sizes[k]);
    data(&w, 8);
    CHECK_EQ_INT(parse(&w, w.n, &i), WAV_ERR_NO_DATA);
  }

  // Chunk header split by the end of the buffer.
  riff(&w);
  fmt(&w, 1, 2, 44100, 16);
  data(&w, 8);
  CHECK_EQ_INT(parse(&w, 36 + 7, &i), WAV_ERR_NO_DATA);
}

static void test_data_size(void) {
  wbuf_t w;
  wav_info_t i;
  // Partial trailing frame rounded down.
  riff(&w);
  fmt(&w, 1, 2, 44100, 16);
  data(&w, 10);
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_OK);
  CHECK_EQ_U32(i.data_size, 8);
  // Declared size past the buffer is reported as declared (callers clamp).
  riff(&w);
  fmt(&w, 1, 1, 8000, 8);
  chunk(&w, "data", 0xFFFFFFFFu);  // "unknown length" streaming header
  CHECK_EQ_INT(parse(&w, w.n, &i), WAV_OK);
  CHECK_EQ_U32(i.data_size, 0xFFFFFFFFu);
  CHECK_EQ_U32(i.data_offset, 44);
}

static void test_real_files(void) {
  static const char *k_files[] = {
      "apps/guinea_pig/sfx/bank1.wav", "apps/guinea_pig/sfx/bank2.wav",
      "apps/guinea_pig/sfx/bgm.wav", "apps/panels_demo/audio/bgm.wav",
      "apps/panels_demo/audio/chime.wav", "tests/fuzz/corpus/wav/list_ext.wav",
      "tests/fuzz/corpus/wav/canonical.wav"};
  for (size_t k = 0; k < sizeof(k_files) / sizeof(k_files[0]); k++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", PICODECK_ROOT, k_files[k]);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
      continue;
    uint8_t buf[WAV_HEADER_WINDOW];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    wav_info_t i;
    wav_err_t e = wav_parse(buf, n, &i);
    if (e != WAV_OK)
      printf("  %s: %s\n", k_files[k], wav_strerror(e));
    CHECK_EQ_INT(e, WAV_OK);
    CHECK(i.data_offset + i.data_size <= (uint32_t)size);
    printf("  %s: %u Hz %u-bit %u ch, data @%u (%u bytes)\n", k_files[k],
           (unsigned)i.sample_rate, (unsigned)i.bits_per_sample,
           (unsigned)i.channels, (unsigned)i.data_offset, (unsigned)i.data_size);
  }
}

int main(void) {
  test_canonical();
  test_list_chunks();
  test_extensible();
  test_unsupported();
  test_malformed();
  test_data_size();
  test_real_files();
  for (int e = WAV_OK; e <= WAV_ERR_NO_DATA; e++)
    CHECK(wav_strerror((wav_err_t)e)[0] != '\0');
  return check_report("test_wav");
}
