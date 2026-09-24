// pio_psram_xip_split(): how a DMA read into the XIP-cached PSRAM window is
// cut into a CPU-copied head, whole cache lines for the DMA (invalidated
// before and after), and a CPU-copied tail. Only whole lines inside the
// buffer may be invalidated: a line shared with a neighbour could hold its
// pending writes.
#include "check.h"
#include "pio_psram_xip.h"

static void check_split(uintptr_t dst, uint32_t len) {
  pio_psram_xip_split_t s = pio_psram_xip_split(dst, len);
  CHECK_EQ_U32(s.head + s.body + s.tail, len);
  CHECK(s.head < PIO_PSRAM_XIP_LINE);
  CHECK(s.tail < PIO_PSRAM_XIP_LINE);
  CHECK_EQ_U32(s.body % PIO_PSRAM_XIP_LINE, 0);
  if (s.body) {
    // The body starts on a line boundary and ends inside the buffer.
    CHECK_EQ_U32((dst + s.head) % PIO_PSRAM_XIP_LINE, 0);
    CHECK(dst + s.head + s.body <= dst + len);
    // Nothing that could have been a whole line was left to the CPU.
    CHECK(s.head == 0 || (dst + s.head) % PIO_PSRAM_XIP_LINE == 0);
  } else {
    // No whole line fits.
    uintptr_t first = (dst + PIO_PSRAM_XIP_LINE - 1) & ~(uintptr_t)(PIO_PSRAM_XIP_LINE - 1);
    CHECK(first + PIO_PSRAM_XIP_LINE > dst + len);
  }
}

static void test_split(void) {
  for (uintptr_t base = 0x11200000u; base < 0x11200000u + 16; base++)
    for (uint32_t len = 0; len < 64; len++)
      check_split(base, len);
  check_split(0x11200003u, 1024u * 1024u);

  pio_psram_xip_split_t s = pio_psram_xip_split(0x11200000u, 256);
  CHECK_EQ_U32(s.head, 0);  CHECK_EQ_U32(s.body, 256);  CHECK_EQ_U32(s.tail, 0);
  s = pio_psram_xip_split(0x11200003u, 256);
  CHECK_EQ_U32(s.head, 5);  CHECK_EQ_U32(s.body, 248);  CHECK_EQ_U32(s.tail, 3);
  s = pio_psram_xip_split(0x11200003u, 4);
  CHECK_EQ_U32(s.head, 4);  CHECK_EQ_U32(s.body, 0);    CHECK_EQ_U32(s.tail, 0);
  s = pio_psram_xip_split(0x11200006u, 12);
  CHECK_EQ_U32(s.head, 2);  CHECK_EQ_U32(s.body, 8);    CHECK_EQ_U32(s.tail, 2);
}

static void test_cached_window(void) {
  CHECK(pio_psram_xip_is_cached(0x11200000u));
  CHECK(pio_psram_xip_is_cached(0x11FFFFFFu));
  CHECK(!pio_psram_xip_is_cached(0x15200000u));  // uncached alias
  CHECK(!pio_psram_xip_is_cached(0x20001000u));  // SRAM
  CHECK(!pio_psram_xip_is_cached(0x10001000u));  // flash
}

int main(void) {
  test_split();
  test_cached_window();
  return check_report("test_pio_psram_xip");
}
