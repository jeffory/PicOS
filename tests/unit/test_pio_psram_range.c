// pio_psram_app_range_check(): the OS region below PIO_PSRAM_APP_BASE and
// anything past the chip are refused, in 64-bit arithmetic.
#include "check.h"
#include "pio_psram.h"

#define CHIP (8u * 1024u * 1024u)

static void test_range(void) {
  CHECK_EQ_INT(pio_psram_app_range_check(PIO_PSRAM_APP_BASE, 1, CHIP), PIO_PSRAM_RANGE_OK);
  CHECK_EQ_INT(pio_psram_app_range_check(CHIP - 1, 1, CHIP), PIO_PSRAM_RANGE_OK);
  CHECK_EQ_INT(pio_psram_app_range_check(PIO_PSRAM_APP_BASE, CHIP - PIO_PSRAM_APP_BASE, CHIP),
               PIO_PSRAM_RANGE_OK);
  CHECK_EQ_INT(pio_psram_app_range_check(CHIP, 0, CHIP), PIO_PSRAM_RANGE_OK);

  // The OS region: MP3 ring, video pool, and a range straddling the base.
  CHECK_EQ_INT(pio_psram_app_range_check(PIO_PSRAM_MP3_RING_BASE, 1, CHIP), PIO_PSRAM_RANGE_RESERVED);
  CHECK_EQ_INT(pio_psram_app_range_check(PIO_PSRAM_VIDEO_BASE, 16, CHIP), PIO_PSRAM_RANGE_RESERVED);
  CHECK_EQ_INT(pio_psram_app_range_check(PIO_PSRAM_APP_BASE - 1, 2, CHIP), PIO_PSRAM_RANGE_RESERVED);
  CHECK_EQ_INT(pio_psram_app_range_check(0, 0, CHIP), PIO_PSRAM_RANGE_RESERVED);

  // Negative and past-the-end, including values that would wrap in 32 bits.
  CHECK_EQ_INT(pio_psram_app_range_check(-1, 1, CHIP), PIO_PSRAM_RANGE_OUT_OF_RANGE);
  CHECK_EQ_INT(pio_psram_app_range_check(PIO_PSRAM_APP_BASE, -1, CHIP), PIO_PSRAM_RANGE_OUT_OF_RANGE);
  CHECK_EQ_INT(pio_psram_app_range_check(CHIP - 1, 2, CHIP), PIO_PSRAM_RANGE_OUT_OF_RANGE);
  CHECK_EQ_INT(pio_psram_app_range_check(0xFFFFFFFFll, 2, CHIP), PIO_PSRAM_RANGE_OUT_OF_RANGE);
  CHECK_EQ_INT(pio_psram_app_range_check(PIO_PSRAM_APP_BASE, 0x100000000ll, CHIP),
               PIO_PSRAM_RANGE_OUT_OF_RANGE);
  CHECK_EQ_INT(pio_psram_app_range_check(INT64_MAX, INT64_MAX, CHIP), PIO_PSRAM_RANGE_OUT_OF_RANGE);
  CHECK_EQ_INT(pio_psram_app_range_check(PIO_PSRAM_APP_BASE, INT64_MAX, CHIP),
               PIO_PSRAM_RANGE_OUT_OF_RANGE);
}

int main(void) {
  test_range();
  return check_report("test_pio_psram_range");
}
