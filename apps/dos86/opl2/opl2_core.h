/*
  Minimal OPL2 FM synthesis core.

  Emulates a Yamaha YM3812 (OPL2) — 9 two-operator FM channels with ADSR
  envelopes and 4 waveforms. Sufficient for AdLib-compatible DOS games.

  Not cycle-accurate; trades precision for performance on Cortex-M33 @ 200MHz.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#ifndef OPL2_CORE_H
#define OPL2_CORE_H

#include <stdint.h>

/* Number of OPL2 channels and operators */
#define OPL2_NUM_CHANNELS   9
#define OPL2_NUM_OPERATORS  18

/* ADSR envelope states */
typedef enum {
    ENV_OFF = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
} opl2_env_state_t;

/* Single operator state */
typedef struct {
    /* Register parameters */
    uint8_t  am;            /* Amplitude modulation (tremolo) */
    uint8_t  vib;           /* Vibrato */
    uint8_t  egt;           /* Envelope type (1=sustained, 0=decaying) */
    uint8_t  ksr;           /* Key scale rate */
    uint8_t  mult;          /* Frequency multiplier (0-15) */
    uint8_t  ksl;           /* Key scale level */
    uint8_t  tl;            /* Total level (attenuation, 0-63) */
    uint8_t  ar;            /* Attack rate (0-15) */
    uint8_t  dr;            /* Decay rate (0-15) */
    uint8_t  sl;            /* Sustain level (0-15) */
    uint8_t  rr;            /* Release rate (0-15) */
    uint8_t  ws;            /* Waveform select (0-3) */

    /* Runtime state */
    opl2_env_state_t env_state;
    uint32_t env_level;     /* Current envelope level (0=max, 0x1FF=min) */
    uint32_t phase;         /* Phase accumulator (fixed-point 10.22) */
    int32_t  feedback_out[2]; /* Last two output samples for feedback */
} opl2_operator_t;

/* Channel state */
typedef struct {
    uint16_t fnum;          /* Frequency number (0-1023) */
    uint8_t  block;         /* Block/octave (0-7) */
    uint8_t  key_on;        /* Key on flag */
    uint8_t  feedback;      /* Feedback level (0-7) */
    uint8_t  algorithm;     /* 0=FM, 1=additive */
} opl2_channel_t;

/* Full OPL2 chip state */
typedef struct {
    opl2_operator_t ops[OPL2_NUM_OPERATORS];
    opl2_channel_t  channels[OPL2_NUM_CHANNELS];

    uint8_t  regs[256];     /* Raw register mirror */
    uint8_t  reg_select;    /* Currently selected register */

    /* Timer state for status register */
    uint8_t  status;        /* Status byte returned on port 0x388 read */
    uint8_t  timer1_val;
    uint8_t  timer2_val;
    uint8_t  timer_ctrl;    /* IRQ reset / mask / start bits */

    /* Global controls */
    uint8_t  waveform_enable; /* Register 0x01 bit 5: enable waveform select */
    uint8_t  am_depth;      /* Tremolo depth (reg 0xBD bit 7) */
    uint8_t  vib_depth;     /* Vibrato depth (reg 0xBD bit 6) */

    /* Output parameters */
    uint32_t sample_rate;
    uint32_t phase_step_base; /* Pre-computed base for phase calculation */

    /* Tremolo/vibrato LFO */
    uint32_t lfo_counter;
} opl2_chip_t;

/* Initialize chip state for given sample rate */
void opl2_core_init(opl2_chip_t *chip, uint32_t sample_rate);

/* Write a value to an OPL2 register */
void opl2_core_write_reg(opl2_chip_t *chip, uint8_t reg, uint8_t val);

/* Generate num_samples of mono int16_t PCM */
void opl2_core_generate(opl2_chip_t *chip, int16_t *buf, int num_samples);

#endif /* OPL2_CORE_H */
