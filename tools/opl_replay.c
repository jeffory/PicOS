// OPL replay tool — reads .opcl capture, writes 16-bit stereo WAV
//
// Two modes:
//   Default: replay through our opl.c at 11025 Hz (same as on-device)
//   --native: replay at 49716 Hz with f_num unscaling (reference quality)
//
// Build:
//   gcc -O2 -o opl_replay tools/opl_replay.c apps/doom/opl.c -I apps/doom -lm
//
// Usage:
//   ./opl_replay opl_dump.opcl output.wav           # 11025 Hz (our emulator)
//   ./opl_replay --native opl_dump.opcl output.wav  # 49716 Hz (native rate)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "opl.h"

// WAV header (44 bytes)
static void write_wav_header(FILE *f, uint32_t data_size, uint32_t rate)
{
    uint32_t file_size = 36 + data_size;
    uint16_t channels = 2;
    uint16_t bits = 16;
    uint32_t byte_rate = rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);

    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_fmt = 1; // PCM
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
}

// Per-channel f_num tracking for native mode unscaling
static uint16_t chan_fnum[9];   // Current f_num per channel (from 0xA0 writes)
static uint8_t  chan_block[9];  // Current block per channel

// Unscale f_num: captured values were multiplied by 49716/11025 in mus_player.c
// We need to reverse that scaling and re-normalize block/f_num
static void unscale_fnum(uint8_t ch, uint16_t scaled_fnum, uint8_t block,
                          uint16_t *out_fnum, uint8_t *out_block)
{
    // Reconstruct the full frequency value
    uint32_t freq = (uint32_t)scaled_fnum << block;

    // Reverse the 49716/11025 scaling
    freq = freq * 11025 / 49716;

    // Renormalize into f_num (10-bit) + block (3-bit)
    uint8_t b = 0;
    while (freq > 1023 && b < 7) {
        freq >>= 1;
        b++;
    }
    if (freq > 1023) freq = 1023;

    *out_fnum = (uint16_t)freq;
    *out_block = b;
}

int main(int argc, char *argv[])
{
    int native_mode = 0;
    int argi = 1;

    if (argi < argc && strcmp(argv[argi], "--native") == 0) {
        native_mode = 1;
        argi++;
    }

    if (argc - argi != 2) {
        fprintf(stderr, "Usage: %s [--native] input.opcl output.wav\n", argv[0]);
        fprintf(stderr, "  --native: replay at 49716 Hz (reference quality)\n");
        return 1;
    }

    const char *infile = argv[argi];
    const char *outfile = argv[argi + 1];

    FILE *fin = fopen(infile, "rb");
    if (!fin) {
        perror(infile);
        return 1;
    }

    // Read and validate header
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, fin) != 16) {
        fprintf(stderr, "Error: truncated header\n");
        fclose(fin);
        return 1;
    }
    if (memcmp(hdr, "OPCL", 4) != 0) {
        fprintf(stderr, "Error: bad magic (expected OPCL)\n");
        fclose(fin);
        return 1;
    }

    uint16_t version = hdr[4] | (hdr[5] << 8);
    uint16_t cap_rate = hdr[6] | (hdr[7] << 8);
    uint16_t mus_hz  = hdr[8] | (hdr[9] << 8);

    // In native mode, output at 49716 Hz; otherwise use capture rate
    uint32_t out_rate = native_mode ? 49716 : cap_rate;
    // Ratio: how many output samples per captured sample
    // In native mode: 49716/11025 ≈ 4.51
    // In normal mode: 1.0
    double rate_ratio = (double)out_rate / cap_rate;

    printf("OPCL v%d, capture_rate=%d, mus_hz=%d\n", version, cap_rate, mus_hz);
    printf("Mode: %s, output_rate=%u\n", native_mode ? "NATIVE 49716Hz" : "11025Hz", out_rate);

    FILE *fout = fopen(outfile, "wb");
    if (!fout) {
        perror(outfile);
        fclose(fin);
        return 1;
    }
    write_wav_header(fout, 0, out_rate);

    // Initialize OPL
    opl3_chip chip;
    OPL3_Reset(&chip, out_rate);
    OPL3_WriteRegBuffered(&chip, 0x01, 0x20);

    memset(chan_fnum, 0, sizeof(chan_fnum));
    memset(chan_block, 0, sizeof(chan_block));

    uint64_t total_output_samples = 0;
    uint32_t total_writes = 0;
    int done = 0;
    int16_t peak_raw = 0;

    // Gain: OPL2 at 11025Hz produces ~400 peak.  24× matches on-device mixing.
    // Native mode needs less since it runs at proper rate.
    int gain = native_mode ? 8 : 24;

    // PCM buffer
    int16_t pcm[1024];

    while (!done) {
        uint8_t rec[4];
        if (fread(rec, 1, 4, fin) != 4)
            break;

        switch (rec[0]) {
        case 0x01: {  // Register write
            uint8_t reg = rec[1];
            uint8_t val = rec[2];

            if (native_mode) {
                // Intercept f_num writes to unscale them
                if (reg >= 0xA0 && reg <= 0xA8) {
                    // F-Num low byte — store for later
                    uint8_t ch = reg - 0xA0;
                    chan_fnum[ch] = (chan_fnum[ch] & 0x300) | val;
                    // Don't write yet — we'll write when 0xB0 comes
                    // (but write now in case it doesn't come)
                    uint16_t uf; uint8_t ub;
                    unscale_fnum(ch, chan_fnum[ch], chan_block[ch], &uf, &ub);
                    OPL3_WriteRegBuffered(&chip, reg, uf & 0xFF);
                    total_writes++;
                    break;
                }
                if (reg >= 0xB0 && reg <= 0xB8) {
                    uint8_t ch = reg - 0xB0;
                    uint8_t key_on = val & 0x20;
                    chan_block[ch] = (val >> 2) & 7;
                    chan_fnum[ch] = (chan_fnum[ch] & 0xFF) | ((uint16_t)(val & 3) << 8);

                    uint16_t uf; uint8_t ub;
                    unscale_fnum(ch, chan_fnum[ch], chan_block[ch], &uf, &ub);

                    uint8_t new_val = key_on | (ub << 2) | ((uf >> 8) & 3);
                    OPL3_WriteRegBuffered(&chip, 0xA0 + ch, uf & 0xFF);
                    OPL3_WriteRegBuffered(&chip, reg, new_val);
                    total_writes += 2;
                    break;
                }
            }

            OPL3_WriteRegBuffered(&chip, reg, val);
            total_writes++;
            break;
        }

        case 0x02: {  // Advance N samples (at capture rate)
            int cap_count = rec[1];
            if (cap_count == 0) cap_count = 1;

            // Calculate output samples
            int out_count;
            if (native_mode) {
                out_count = (int)(cap_count * rate_ratio + 0.5);
            } else {
                out_count = cap_count;
            }
            total_output_samples += out_count;

            while (out_count > 0) {
                int batch = out_count > 512 ? 512 : out_count;
                if (native_mode) {
                    // At native rate, just call Generate directly
                    // (no rate compensation needed — 1:1)
                    for (int i = 0; i < batch; i++) {
                        OPL3_Generate(&chip, &pcm[i * 2]);
                    }
                } else {
                    OPL3_GenerateBatch(&chip, pcm, batch);
                }
                // Apply gain and track peak
                for (int j = 0; j < batch * 2; j++) {
                    int16_t raw = pcm[j];
                    if (abs(raw) > peak_raw) peak_raw = abs(raw);
                    int32_t s = (int32_t)raw * gain;
                    if (s > 32767) s = 32767;
                    if (s < -32768) s = -32768;
                    pcm[j] = (int16_t)s;
                }
                fwrite(pcm, sizeof(int16_t), batch * 2, fout);
                out_count -= batch;
            }
            break;
        }

        case 0xFF:  // End of stream
            done = 1;
            break;

        default:
            fprintf(stderr, "Warning: unknown record type 0x%02x at offset %ld\n",
                    rec[0], ftell(fin) - 4);
            break;
        }
    }

    fclose(fin);

    // Fix WAV header with actual data size
    uint32_t data_size = (uint32_t)(total_output_samples * 2 * sizeof(int16_t));
    fseek(fout, 0, SEEK_SET);
    write_wav_header(fout, data_size, out_rate);
    fclose(fout);

    float duration = (float)total_output_samples / out_rate;
    printf("Peak raw amplitude: %d (gain=%dx, output peak=%d)\n",
           peak_raw, gain, peak_raw * gain > 32767 ? 32767 : peak_raw * gain);
    printf("Done: %llu samples (%.1fs), %u reg writes\n",
           (unsigned long long)total_output_samples, duration, total_writes);
    printf("Output: %s (%u bytes)\n", outfile, 44 + data_size);

    return 0;
}
