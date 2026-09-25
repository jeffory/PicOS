#pragma once

// RIFF/WAVE header parser shared by the sample loader (sound.c), the
// streaming file player (fileplayer.c) and the simulator's copies of both
// (simulator/sim_audio.c).  Pure: parses a caller-supplied buffer holding
// the start of the file.  Host-tested in tests/unit/test_wav.c and fuzzed by
// tests/fuzz/fuzz_wav.c.
//
// Walks the RIFF chunk list, so LIST/INFO/fact chunks before "data" and
// WAVE_FORMAT_EXTENSIBLE headers are handled; only uncompressed PCM with 1-2
// channels and 8 or 16 bits per sample is accepted (what the players can
// play).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bytes a streaming caller should read from the start of the file for
// wav_parse(): room for a 40-byte EXTENSIBLE fmt chunk plus LIST/INFO
// metadata before the data chunk.
#define WAV_HEADER_WINDOW 512u

typedef struct {
    uint16_t channels;         // 1 or 2
    uint16_t bits_per_sample;  // 8 or 16
    uint32_t sample_rate;      // Hz, 1..192000
    uint16_t block_align;      // bytes per frame = channels * bits / 8
    uint32_t data_offset;      // file offset of the first sample byte
    uint32_t data_size;        // bytes the data chunk declares, rounded down
                               // to whole frames (may run past the end of
                               // the file: callers clamp to what they read)
} wav_info_t;

typedef enum {
    WAV_OK = 0,
    WAV_ERR_NOT_WAV,       // too short or not RIFF....WAVE
    WAV_ERR_NO_FMT,        // no "fmt " chunk before "data"
    WAV_ERR_BAD_FMT,       // fmt chunk shorter than 16 bytes / truncated
    WAV_ERR_UNSUPPORTED,   // not PCM, >2 channels, not 8/16-bit, bad rate
    WAV_ERR_NO_DATA,       // no "data" chunk inside the buffer
} wav_err_t;

wav_err_t wav_parse(const uint8_t *buf, size_t len, wav_info_t *out);

const char *wav_strerror(wav_err_t err);
