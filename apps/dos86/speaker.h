/*
  DOS86 — PC Speaker square wave synthesizer.

  Driven by PIT channel 2 frequency.  Port 0x61 bits 0+1 gate the speaker:
    bit 0: PIT channel 2 gate enable
    bit 1: speaker output enable
  Both bits must be set for sound to be produced.

  Usage:
    speaker_init()                       — call once at startup
    speaker_set_gate(bool)               — call from port 0x61 handler
    speaker_generate(buf, n, rate)       — fill int16_t PCM buffer each frame

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#ifndef DOS86_SPEAKER_H
#define DOS86_SPEAKER_H

#include <stdint.h>
#include <stdbool.h>

/* Initialize speaker state (call once). */
void speaker_init(void);

/* Enable or disable speaker gate (from port 0x61, bits 0+1 both set). */
void speaker_set_gate(bool enabled);

/* Fill 'num_samples' stereo int16_t samples into buf at the given sample_rate.
   buf must hold at least num_samples * 2 int16_t values (L, R interleaved).
   Silence is written when gate is off or frequency is 0. */
void speaker_generate(int16_t *buf, int num_samples, uint32_t sample_rate);

#endif /* DOS86_SPEAKER_H */
