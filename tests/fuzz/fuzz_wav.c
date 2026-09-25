// libFuzzer target for src/drivers/wav.c: the RIFF/WAVE header walk over
// arbitrary bytes (exactly `size` bytes, so ASan catches overreads; the
// libFuzzer timeout catches a chunk walk that stops advancing).  On success
// the reported layout must be self-consistent.
#include "wav.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  wav_info_t info;
  if (wav_parse(data, size, &info) != WAV_OK)
    return 0;
  if (info.data_offset > size || info.block_align == 0 ||
      info.block_align != info.channels * (info.bits_per_sample / 8) ||
      info.data_size % info.block_align != 0 ||
      info.channels < 1 || info.channels > 2 || info.sample_rate == 0)
    abort();
  return 0;
}
