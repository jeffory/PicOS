#include "wav.h"

#include <string.h>

#define WAVE_FORMAT_PCM        1u
#define WAVE_FORMAT_EXTENSIBLE 0xFFFEu

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

const char *wav_strerror(wav_err_t err) {
    switch (err) {
    case WAV_OK:              return "ok";
    case WAV_ERR_NOT_WAV:     return "not a RIFF/WAVE file";
    case WAV_ERR_NO_FMT:      return "no fmt chunk before data";
    case WAV_ERR_BAD_FMT:     return "malformed fmt chunk";
    case WAV_ERR_UNSUPPORTED: return "unsupported format (need 8/16-bit PCM, 1-2 ch)";
    case WAV_ERR_NO_DATA:     return "no data chunk";
    }
    return "unknown error";
}

wav_err_t wav_parse(const uint8_t *buf, size_t len, wav_info_t *out) {
    if (!buf || !out || len < 12 || memcmp(buf, "RIFF", 4) != 0 ||
        memcmp(buf + 8, "WAVE", 4) != 0)
        return WAV_ERR_NOT_WAV;

    bool have_fmt = false;
    uint16_t format = 0;
    memset(out, 0, sizeof(*out));

    size_t pos = 12;
    while (pos <= len && len - pos >= 8) {
        const uint8_t *ck = buf + pos;
        uint32_t size = rd32(ck + 4);
        size_t body = pos + 8;

        if (memcmp(ck, "fmt ", 4) == 0) {
            if (size < 16 || len - body < 16)
                return WAV_ERR_BAD_FMT;
            const uint8_t *f = buf + body;
            format = rd16(f);
            out->channels = rd16(f + 2);
            out->sample_rate = rd32(f + 4);
            out->bits_per_sample = rd16(f + 14);
            // EXTENSIBLE: cbSize(2) validBits(2) channelMask(4) SubFormat
            // GUID, whose first two bytes are the real format tag.
            if (format == WAVE_FORMAT_EXTENSIBLE) {
                if (size < 40 || len - body < 26)
                    return WAV_ERR_BAD_FMT;
                format = rd16(f + 24);
            }
            have_fmt = true;
        } else if (memcmp(ck, "data", 4) == 0) {
            if (!have_fmt)
                return WAV_ERR_NO_FMT;
            if (format != WAVE_FORMAT_PCM ||
                out->channels < 1 || out->channels > 2 ||
                (out->bits_per_sample != 8 && out->bits_per_sample != 16) ||
                out->sample_rate == 0 || out->sample_rate > 192000)
                return WAV_ERR_UNSUPPORTED;
            out->block_align =
                (uint16_t)(out->channels * (out->bits_per_sample / 8));
            out->data_offset = (uint32_t)body;
            out->data_size = size - size % out->block_align;
            return WAV_OK;
        }

        // Next chunk: bodies are padded to an even length.  Stop (rather
        // than wrap: size_t is 32 bits on the device) when the chunk runs
        // past the buffer.
        if (size > len - body)
            break;
        pos = body + size + (size & 1u);  // may be len + 1: loop ends
    }
    return have_fmt ? WAV_ERR_NO_DATA : WAV_ERR_NO_FMT;
}
