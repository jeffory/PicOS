// Nuked OPL3 emulator - minimal OPL2-mode interface for DOOM music
// Based on Nuked OPL3 by Nuke.YKT (Alexey Khokholov)
// Original source: https://github.com/nukeykt/Nuked-OPL3
//
// Copyright (C) 2013-2020 Nuke.YKT
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

#ifndef OPL_H
#define OPL_H

#include <stdint.h>

// OPL3 chip state (~4.4KB)
typedef struct {
    uint32_t chip_generate_count;
    // Slot state (36 slots for OPL3, we use 18 for OPL2)
    struct opl_slot {
        int16_t  out;
        int16_t  fbmod;
        int16_t *mod;
        int16_t  eg_rout;
        int16_t  eg_out;
        uint8_t  eg_inc;
        uint8_t  eg_gen;
        uint8_t  eg_rate;
        uint8_t  eg_ksl;
        uint8_t *trem;
        uint8_t  reg_vib;
        uint8_t  reg_type;
        uint8_t  reg_ksr;
        uint8_t  reg_mult;
        uint8_t  reg_ksl_tl;
        uint8_t  reg_tl;
        uint8_t  reg_ar;
        uint8_t  reg_dr;
        uint8_t  reg_sl;
        uint8_t  reg_rr;
        uint8_t  reg_wf;
        uint8_t  key;
        uint32_t pg_reset;
        uint32_t pg_phase;
        uint16_t pg_phase_out;
        uint8_t  slot_num;
    } slot[36];
    // Channel state (18 channels for OPL3, we use 9 for OPL2)
    struct opl_channel {
        struct opl_slot *slots[2];
        struct opl_channel *pair;
        uint16_t f_num;
        uint8_t  block;
        uint8_t  fb;
        uint8_t  con;
        uint8_t  alg;
        uint8_t  ksv;
        uint16_t cha, chb;
        uint8_t  ch_num;
    } channel[18];
    // Global state
    uint16_t timer;
    uint64_t eg_timer;
    uint8_t  eg_timerrem;
    uint8_t  eg_state;
    uint8_t  eg_add;
    uint8_t  newm;            // OPL3 new mode (0 for OPL2)
    uint8_t  nts;
    uint8_t  rhy;
    uint8_t  vibpos;
    uint8_t  vibshift;
    uint8_t  tremolo;
    uint8_t  tremolopos;
    uint8_t  tremoloshift;
    uint32_t noise;
    int16_t  zeromod;
    int32_t  mixbuff[2];
    uint8_t  rm_hh_bit2;
    uint8_t  rm_hh_bit3;
    uint8_t  rm_hh_bit7;
    uint8_t  rm_hh_bit8;
    uint8_t  rm_tc_bit3;
    uint8_t  rm_tc_bit5;
    // Write buffer for register writes
    uint8_t  writebuf_samplecnt;
    // Rhythm mode slots cache
    uint8_t  rhy_flag;
} opl3_chip;

// Initialize OPL3 chip in OPL2 mode
void OPL3_Reset(opl3_chip *chip, uint32_t samplerate);

// Write a value to an OPL register (port 0 = addr, port 1 = data)
void OPL3_WriteRegBuffered(opl3_chip *chip, uint16_t reg, uint8_t v);

// Generate one stereo sample pair
void OPL3_GenerateResampled(opl3_chip *chip, int16_t *buf);

// Direct generation at native rate
void OPL3_Generate(opl3_chip *chip, int16_t *buf);

#endif // OPL_H
