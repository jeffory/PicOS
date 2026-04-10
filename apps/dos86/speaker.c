/*
  DOS86 — PC Speaker square wave synthesizer.

  Uses a fixed-point phase accumulator to generate a square wave at the
  frequency supplied by PIT channel 2.  The phase step is:

      step = (freq << 16) / sample_rate

  Each output sample flips the polarity once the 16-bit phase accumulator
  rolls over 0x8000 (half-cycle boundary).

  Amplitude is ±4096 (about 12.5% of int16_t range) to avoid clipping when
  mixed with other audio sources.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#include "speaker.h"
#include "fake86/i8253.h"
#include <stdint.h>
#include <stdbool.h>

#define SPEAKER_AMPLITUDE  4096

static bool     s_gate;        /* true when port 0x61 bits 0+1 are both set  */
static uint32_t s_phase;       /* fixed-point phase accumulator (16.16)       */
static int16_t  s_polarity;    /* current square-wave polarity (+1 or -1)     */

void speaker_init(void)
{
    s_gate     = false;
    s_phase    = 0;
    s_polarity = 1;
}

void speaker_set_gate(bool enabled)
{
    s_gate = enabled;
    if (!enabled) {
        /* Reset phase so next enable starts cleanly. */
        s_phase    = 0;
        s_polarity = 1;
    }
}

void speaker_generate(int16_t *buf, int num_samples, uint32_t sample_rate)
{
    uint32_t freq = s_gate ? i8253_get_speaker_freq() : 0u;

    if (!s_gate || freq == 0 || sample_rate == 0) {
        /* Silence */
        for (int i = 0; i < num_samples * 2; i++)
            buf[i] = 0;
        return;
    }

    /* Phase step in 16.16 fixed point: how much phase advances per sample. */
    /* step = (freq << 16) / sample_rate                                     */
    uint32_t step = (uint32_t)(((uint64_t)freq << 16) / sample_rate);

    int16_t amplitude = (int16_t)(s_polarity * SPEAKER_AMPLITUDE);

    for (int i = 0; i < num_samples; i++) {
        uint32_t prev_phase = s_phase;
        s_phase += step;

        /* Detect half-cycle crossing (bit 16 toggles) */
        if ((prev_phase ^ s_phase) & 0x10000u) {
            s_polarity = -s_polarity;
            amplitude  = (int16_t)(s_polarity * SPEAKER_AMPLITUDE);
        }

        /* Stereo interleaved L, R */
        buf[i * 2 + 0] = amplitude;
        buf[i * 2 + 1] = amplitude;
    }
}
