// Nuked OPL3 emulator - OPL2-mode subset for DOOM music on PicOS
// Based on Nuked OPL3 v1.8 by Nuke.YKT (Alexey Khokholov)
// https://github.com/nukeykt/Nuked-OPL3
//
// Copyright (C) 2013-2020 Nuke.YKT
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// Trimmed for integer-only OPL2 (9-voice) use on RP2350.

#include "opl.h"
#include <string.h>

// --- Tables ---

// Envelope generator increment table
static const uint8_t eg_incstep[4][4] = {
    { 0, 0, 0, 0 },
    { 1, 0, 0, 0 },
    { 1, 0, 1, 0 },
    { 1, 1, 1, 0 }
};

// Multiplier table (x2)
static const uint8_t mt[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

// KSL table
static const uint8_t kslrom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};

static const uint8_t kslshift[4] = {
    8, 1, 2, 0
};

// Envelope generator rates
static const uint8_t eg_incdesc[16] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2
};

static const int8_t eg_incsh[16] = {
    0, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, -1, -2
};

// Log-sin table (quarter wave, 256 entries, 12-bit)
static const uint16_t logsinrom[256] = {
    2137, 1731, 1543, 1419, 1326, 1252, 1190, 1137,
    1091, 1050, 1013,  979,  949,  920,  894,  869,
     846,  825,  804,  785,  767,  749,  732,  717,
     701,  687,  672,  659,  646,  633,  621,  609,
     598,  587,  576,  566,  556,  546,  536,  527,
     518,  509,  501,  492,  484,  476,  468,  461,
     453,  446,  439,  432,  425,  418,  411,  405,
     399,  392,  386,  380,  375,  369,  363,  358,
     352,  347,  341,  336,  331,  326,  321,  316,
     311,  307,  302,  297,  293,  289,  284,  280,
     276,  271,  267,  263,  259,  255,  251,  248,
     244,  240,  236,  233,  229,  226,  222,  219,
     215,  212,  209,  205,  202,  199,  196,  193,
     190,  187,  184,  181,  178,  175,  172,  169,
     167,  164,  161,  159,  156,  153,  151,  148,
     146,  143,  141,  138,  136,  134,  131,  129,
     127,  125,  122,  120,  118,  116,  114,  112,
     110,  108,  106,  104,  102,  100,   98,   96,
      94,   92,   91,   89,   87,   85,   83,   82,
      80,   78,   77,   75,   74,   72,   70,   69,
      67,   66,   64,   63,   62,   60,   59,   57,
      56,   55,   53,   52,   51,   49,   48,   47,
      46,   45,   43,   42,   41,   40,   39,   38,
      37,   36,   35,   34,   33,   32,   31,   30,
      29,   28,   27,   26,   25,   24,   23,   23,
      22,   21,   20,   20,   19,   18,   17,   17,
      16,   15,   15,   14,   13,   13,   12,   12,
      11,   10,   10,    9,    9,    8,    8,    7,
       7,    7,    6,    6,    5,    5,    5,    4,
       4,    4,    3,    3,    3,    2,    2,    2,
       2,    1,    1,    1,    1,    1,    1,    1,
       0,    0,    0,    0,    0,    0,    0,    0
};

// Exp table (256 entries, 13-bit)
static const uint16_t exprom[256] = {
       0,    3,    6,    8,   11,   14,   17,   20,
      22,   25,   28,   31,   34,   37,   40,   42,
      45,   48,   51,   54,   57,   60,   63,   66,
      69,   72,   75,   78,   81,   84,   87,   90,
      93,   96,   99,  102,  105,  108,  111,  114,
     117,  120,  123,  126,  130,  133,  136,  139,
     142,  145,  148,  152,  155,  158,  161,  164,
     168,  171,  174,  177,  181,  184,  187,  190,
     194,  197,  200,  204,  207,  210,  214,  217,
     220,  224,  227,  231,  234,  237,  241,  244,
     248,  251,  255,  258,  262,  265,  268,  272,
     276,  279,  283,  286,  290,  293,  297,  300,
     304,  308,  311,  315,  318,  322,  326,  329,
     333,  337,  340,  344,  348,  352,  355,  359,
     363,  367,  370,  374,  378,  382,  385,  389,
     393,  397,  401,  405,  409,  412,  416,  420,
     424,  428,  432,  436,  440,  444,  448,  452,
     456,  460,  464,  468,  472,  476,  480,  484,
     488,  492,  496,  501,  505,  509,  513,  517,
     521,  526,  530,  534,  538,  542,  547,  551,
     555,  560,  564,  568,  572,  577,  581,  585,
     590,  594,  599,  603,  607,  612,  616,  621,
     625,  630,  634,  639,  643,  648,  652,  657,
     661,  666,  670,  675,  680,  684,  689,  693,
     698,  703,  708,  712,  717,  722,  726,  731,
     736,  741,  745,  750,  755,  760,  765,  770,
     774,  779,  784,  789,  794,  799,  804,  809,
     814,  819,  824,  829,  834,  839,  844,  849,
     854,  859,  864,  869,  874,  880,  885,  890,
     895,  900,  906,  911,  916,  921,  927,  932,
     937,  942,  948,  953,  959,  964,  969,  975,
     980,  986,  991,  996, 1002, 1007, 1013, 1018
};

// Slot-channel mapping for OPL2 (first 9 channels only)
// (retained for reference but not used in simplified emulator)

// --- Envelope Generator ---

enum {
    EG_OFF = 0,
    EG_ATTACK,
    EG_DECAY,
    EG_SUSTAIN,
    EG_RELEASE
};

static void OPL3_EnvelopeCalcRate(struct opl_slot *slot,
                                   uint8_t reg_rate,
                                   struct opl_channel *chan)
{
    uint8_t rate;
    if (reg_rate == 0) {
        slot->eg_rate = 0;
        slot->eg_inc = 0;
        return;
    }
    rate = (reg_rate << 2)
         + (slot->reg_ksr ? chan->ksv : (chan->ksv >> 2));
    if (rate > 60)
        rate = 60;
    slot->eg_rate = rate;
}

static void OPL3_EnvelopeUpdateRate(struct opl_slot *slot,
                                     struct opl_channel *chan)
{
    switch (slot->eg_gen) {
    case EG_OFF:
    case EG_ATTACK:
        OPL3_EnvelopeCalcRate(slot, slot->reg_ar, chan);
        break;
    case EG_DECAY:
        OPL3_EnvelopeCalcRate(slot, slot->reg_dr, chan);
        break;
    case EG_SUSTAIN:
    case EG_RELEASE:
        OPL3_EnvelopeCalcRate(slot, slot->reg_rr, chan);
        break;
    }
}

static void OPL3_EnvelopeGenerate(opl3_chip *chip, struct opl_slot *slot)
{
    uint8_t rate_h, rate_l;
    uint8_t inc = 0;

    rate_h = slot->eg_rate >> 2;
    rate_l = slot->eg_rate & 3;

    if (rate_h < 13) {
        int shift = eg_incsh[rate_h];
        if (shift >= 0) {
            if ((chip->eg_timer >> shift) & 1) {
                inc = eg_incstep[rate_l]
                    [((chip->eg_timer >> shift) >> 1) & 3];
            }
        } else {
            inc = eg_incstep[rate_l][chip->eg_timer & 3]
                << (-shift);
        }
    } else {
        inc = eg_incdesc[rate_h] << (rate_l + eg_incsh[rate_h]);
    }

    slot->eg_inc = inc;

    switch (slot->eg_gen) {
    case EG_OFF:
        slot->eg_rout = 0x1ff;
        break;
    case EG_ATTACK:
        if (slot->eg_rout == 0) {
            slot->eg_gen = EG_DECAY;
            // Recalculate rate for decay
            {
                struct opl_channel *chan =
                    &chip->channel[slot->slot_num < 18 ?
                                   slot->slot_num / 2 : 0];
                OPL3_EnvelopeCalcRate(slot, slot->reg_dr, chan);
            }
        } else if (slot->eg_rate >= 60) {
            slot->eg_rout = 0;
        } else {
            slot->eg_rout += ((~(int16_t)slot->eg_rout) * inc) >> 3;
            if (slot->eg_rout < 0)
                slot->eg_rout = 0;
        }
        break;
    case EG_DECAY:
        if ((slot->eg_rout >> 4) >= slot->reg_sl) {
            slot->eg_gen = EG_SUSTAIN;
            {
                struct opl_channel *chan =
                    &chip->channel[slot->slot_num < 18 ?
                                   slot->slot_num / 2 : 0];
                OPL3_EnvelopeCalcRate(slot, slot->reg_rr, chan);
            }
        } else {
            slot->eg_rout += inc;
            if (slot->eg_rout > 0x1ff)
                slot->eg_rout = 0x1ff;
        }
        break;
    case EG_SUSTAIN:
    case EG_RELEASE:
        slot->eg_rout += inc;
        if (slot->eg_rout > 0x1ff)
            slot->eg_rout = 0x1ff;
        break;
    }

    slot->eg_out = slot->eg_rout + (slot->reg_tl << 2)
                 + (slot->eg_ksl >> kslshift[slot->reg_ksl_tl >> 6]);
    if (slot->eg_out > 0x1ff)
        slot->eg_out = 0x1ff;
}

// --- Phase Generator ---

static void OPL3_PhaseGenerate(opl3_chip *chip, struct opl_slot *slot)
{
    uint16_t f_num;
    uint32_t basefreq;
    struct opl_channel *chan = &chip->channel[slot->slot_num < 18 ?
                                              slot->slot_num / 2 : 0];

    f_num = chan->f_num;
    basefreq = f_num << chan->block;

    // Apply vibrato
    if (slot->reg_vib) {
        int8_t range;
        uint8_t vibpos = chip->vibpos;
        // Vibrato depth: 7 cents or 14 cents
        range = (f_num >> (7 - chip->vibshift)) & 7;
        if (vibpos >= 24)
            range = -range;
        else if (vibpos < 8)
            ; // positive
        else if (vibpos < 16)
            ; // positive but decreasing (handled by range value)
        else
            range = -range;
        basefreq += range;
    }

    // Multiply
    slot->pg_phase += (basefreq * mt[slot->reg_mult]) >> 1;
    if (slot->pg_reset) {
        slot->pg_phase = 0;
        slot->pg_reset = 0;
    }
    slot->pg_phase_out = (uint16_t)(slot->pg_phase >> 9);
}

// --- Operator Output ---

static int16_t OPL3_SlotOutput(struct opl_slot *slot, int16_t modulation)
{
    uint16_t phase = (uint16_t)((slot->pg_phase_out + modulation) & 0x3ff);
    uint16_t neg = phase & 0x200;
    uint16_t level;
    int16_t  output;

    if (phase & 0x100)
        phase = (~phase) & 0xff;
    else
        phase &= 0xff;

    // Waveform selection (OPL2: waveforms 0-3)
    switch (slot->reg_wf & 3) {
    case 0: // Sine
        level = logsinrom[phase];
        break;
    case 1: // Half-sine (positive half only)
        if (neg)
            level = 0x1000;
        else
            level = logsinrom[phase];
        break;
    case 2: // Abs-sine (rectified)
        level = logsinrom[phase];
        neg = 0;
        break;
    case 3: // Quarter-sine (pulse)
        if (phase & 0x80)
            level = 0x1000;
        else
            level = logsinrom[phase << 1];
        neg = 0;
        break;
    default:
        level = 0x1000;
        break;
    }

    // Add envelope attenuation
    level += (uint16_t)(slot->eg_out << 3);
    // Add tremolo
    if (slot->trem)
        level += (uint16_t)(*slot->trem);

    if (level >= 0x1000) {
        output = 0;
    } else {
        output = (int16_t)((exprom[level & 0xff] + 1024) >> (level >> 8));
    }

    if (neg)
        output = -output;

    return output;
}

// --- Chip-level processing ---

void OPL3_Reset(opl3_chip *chip, uint32_t samplerate)
{
    (void)samplerate;
    memset(chip, 0, sizeof(opl3_chip));

    // Initialize noise seed
    chip->noise = 1;

    // Default tremolo/vibrato settings
    chip->tremoloshift = 1;
    chip->vibshift = 1;

    // Wire slot-channel connections for OPL2 (9 channels, 18 slots)
    for (int i = 0; i < 9; i++) {
        chip->channel[i].slots[0] = &chip->slot[i * 2];
        chip->channel[i].slots[1] = &chip->slot[i * 2 + 1];
        chip->channel[i].ch_num = i;
        chip->slot[i * 2].slot_num = i * 2;
        chip->slot[i * 2 + 1].slot_num = i * 2 + 1;
    }

    // Default: zero modulation
    chip->zeromod = 0;
    for (int i = 0; i < 18; i++) {
        chip->slot[i].mod = &chip->zeromod;
        chip->slot[i].eg_gen = EG_OFF;
        chip->slot[i].eg_rout = 0x1ff;
        chip->slot[i].eg_out = 0x1ff;
    }
}

// Process one sample at native OPL rate
void OPL3_Generate(opl3_chip *chip, int16_t *buf)
{
    int32_t accm = 0;

    // Advance tremolo
    chip->tremolopos++;
    if (chip->tremolopos >= 210)
        chip->tremolopos = 0;
    chip->tremolo = (uint8_t)((chip->tremolopos < 105 ?
                    chip->tremolopos : (210 - chip->tremolopos))
                    >> chip->tremoloshift);

    // Advance vibrato
    chip->vibpos = (chip->vibpos + 1) & 31;

    // Advance envelope timer
    chip->eg_timer++;
    chip->eg_add = 1;

    // Advance noise
    if (chip->noise & 1)
        chip->noise ^= 0x800302;
    chip->noise >>= 1;

    // Process 9 channels (OPL2 mode)
    for (int ch = 0; ch < 9; ch++) {
        struct opl_channel *chan = &chip->channel[ch];
        struct opl_slot *s0 = chan->slots[0];
        struct opl_slot *s1 = chan->slots[1];

        // Skip channels where both slots have fully decayed
        if (s0->eg_gen == EG_OFF && s1->eg_gen == EG_OFF)
            continue;

        // Phase generation
        OPL3_PhaseGenerate(chip, s0);
        OPL3_PhaseGenerate(chip, s1);

        // Envelope generation
        OPL3_EnvelopeGenerate(chip, s0);
        OPL3_EnvelopeGenerate(chip, s1);

        // Operator output
        int16_t fb = 0;
        if (chan->fb) {
            fb = (s0->out + s0->fbmod) >> (9 - chan->fb);
        }

        int16_t op0_out = OPL3_SlotOutput(s0, fb);
        s0->fbmod = s0->out;
        s0->out = op0_out;

        int16_t out;
        if (chan->con) {
            // Additive: both operators output
            int16_t op1_out = OPL3_SlotOutput(s1, 0);
            s1->out = op1_out;
            out = op0_out + op1_out;
        } else {
            // FM: op0 modulates op1
            int16_t op1_out = OPL3_SlotOutput(s1, op0_out);
            s1->out = op1_out;
            out = op1_out;
        }

        accm += out;
    }

    // Clamp to 16-bit
    if (accm > 32767)  accm = 32767;
    if (accm < -32768) accm = -32768;
    buf[0] = (int16_t)accm;  // Left
    buf[1] = (int16_t)accm;  // Right (mono for OPL2)
}

// Register write handler
void OPL3_WriteRegBuffered(opl3_chip *chip, uint16_t reg, uint8_t v)
{
    // Only handle OPL2 register range (0x00 - 0xFF)
    reg &= 0xFF;

    // Decode register address
    uint8_t high = (reg >> 4) & 0x0F;
    uint8_t low  = reg & 0x0F;

    switch (high) {
    case 0x00:
        switch (reg) {
        case 0x01: // Waveform select enable
            // bit 5: waveform select enable
            break;
        case 0x08: // CSW / Note-Sel
            chip->nts = (v >> 6) & 1;
            break;
        }
        break;

    case 0x02: case 0x03:
        // Timer registers - ignore for emulation
        break;

    case 0x04:
        // Timer control - ignore
        break;

    case 0x02 + 6: // 0x08 handled above
        break;

    default:
        break;
    }

    // Slot registers: 0x20-0x35, 0x40-0x55, 0x60-0x75, 0x80-0x95, 0xE0-0xF5
    // Map register offset to slot number (OPL2: offsets 0x00-0x15)
    {
        // Determine which register group and slot offset
        uint8_t reg_group = 0;
        uint8_t slot_off = 0;
        if (reg >= 0x20 && reg <= 0x35) {
            reg_group = 1; slot_off = reg - 0x20;
        } else if (reg >= 0x40 && reg <= 0x55) {
            reg_group = 2; slot_off = reg - 0x40;
        } else if (reg >= 0x60 && reg <= 0x75) {
            reg_group = 3; slot_off = reg - 0x60;
        } else if (reg >= 0x80 && reg <= 0x95) {
            reg_group = 4; slot_off = reg - 0x80;
        } else if (reg >= 0xE0 && reg <= 0xF5) {
            reg_group = 5; slot_off = reg - 0xE0;
        }

        if (reg_group > 0) {
            // Map slot offset to slot index
            static const int8_t slot_map[22] = {
                 0,  1,  2,  3,  4,  5, -1, -1,
                 6,  7,  8,  9, 10, 11, -1, -1,
                12, 13, 14, 15, 16, 17
            };
            int slot_idx = (slot_off < 22) ? slot_map[slot_off] : -1;
            if (slot_idx >= 0 && slot_idx < 18) {
                struct opl_slot *slot = &chip->slot[slot_idx];
                struct opl_channel *chan = &chip->channel[slot_idx / 2];

                switch (reg_group) {
                case 1: // 0x20: Tremolo/Vibrato/Sustain/KSR/Mult
                    slot->reg_vib  = (v >> 6) & 1;
                    slot->trem = (v & 0x80) ? &chip->tremolo : NULL;
                    slot->reg_type = (v >> 5) & 1;
                    slot->reg_ksr  = (v >> 4) & 1;
                    slot->reg_mult = v & 0x0F;
                    OPL3_EnvelopeUpdateRate(slot, chan);
                    break;

                case 2: // 0x40: KSL/Total Level
                    slot->reg_ksl_tl = v;
                    slot->reg_tl = v & 0x3F;
                    {
                        uint8_t ksl_val = v >> 6;
                        if (ksl_val == 0) {
                            slot->eg_ksl = 0;
                        } else {
                            int16_t ksl = (kslrom[chan->f_num >> 6] << 2)
                                        - ((8 - chan->block) << 5);
                            if (ksl < 0) ksl = 0;
                            slot->eg_ksl = (uint8_t)(ksl >> kslshift[ksl_val]);
                        }
                    }
                    break;

                case 3: // 0x60: Attack/Decay
                    slot->reg_ar = (v >> 4) & 0x0F;
                    slot->reg_dr = v & 0x0F;
                    OPL3_EnvelopeUpdateRate(slot, chan);
                    break;

                case 4: // 0x80: Sustain/Release
                    slot->reg_sl = (v >> 4) & 0x0F;
                    slot->reg_rr = v & 0x0F;
                    OPL3_EnvelopeUpdateRate(slot, chan);
                    break;

                case 5: // 0xE0: Waveform select
                    slot->reg_wf = v & 0x03;
                    break;
                }
            }
        }
    }
    // Channel registers: 0xA0-0xA8, 0xB0-0xB8, 0xC0-0xC8
    if (high >= 0x0A && high <= 0x0C && low < 9) {
        struct opl_channel *chan = &chip->channel[low];

        switch (high) {
        case 0x0A: // F-Num low 8 bits
            chan->f_num = (chan->f_num & 0x300) | v;
            // Update KSV
            chan->ksv = (uint8_t)((chan->block << 1)
                       | ((chan->f_num >> (9 - chip->nts)) & 1));
            OPL3_EnvelopeUpdateRate(chan->slots[0], chan);
            OPL3_EnvelopeUpdateRate(chan->slots[1], chan);
            break;

        case 0x0B: { // Key-On / Block / F-Num high 2 bits
            uint8_t key_on = (v >> 5) & 1;
            chan->block = (v >> 2) & 7;
            chan->f_num = (chan->f_num & 0xFF) | ((uint16_t)(v & 3) << 8);
            chan->ksv = (uint8_t)((chan->block << 1)
                       | ((chan->f_num >> (9 - chip->nts)) & 1));

            // Key on/off for both slots
            for (int s = 0; s < 2; s++) {
                struct opl_slot *slot = chan->slots[s];
                if (key_on && !slot->key) {
                    // Key on
                    slot->key = 1;
                    slot->eg_gen = EG_ATTACK;
                    slot->pg_phase = 0;
                    OPL3_EnvelopeCalcRate(slot, slot->reg_ar, chan);
                    if (slot->eg_rate >= 60) {
                        slot->eg_rout = 0;
                        slot->eg_gen = EG_DECAY;
                        OPL3_EnvelopeCalcRate(slot, slot->reg_dr, chan);
                    }
                } else if (!key_on && slot->key) {
                    // Key off
                    slot->key = 0;
                    if (slot->eg_gen != EG_OFF) {
                        slot->eg_gen = EG_RELEASE;
                        OPL3_EnvelopeCalcRate(slot, slot->reg_rr, chan);
                    }
                }
            }
            OPL3_EnvelopeUpdateRate(chan->slots[0], chan);
            OPL3_EnvelopeUpdateRate(chan->slots[1], chan);
            break;
        }

        case 0x0C: // Feedback / Connection
            chan->fb  = (v >> 1) & 7;
            chan->con = v & 1;
            break;
        }
    }

    // 0xBD: Rhythm mode (percussion) - not used by DOOM, but handle the bit
    if (reg == 0xBD) {
        chip->tremoloshift = (v & 0x80) ? 0 : 1;
        chip->vibshift     = (v & 0x40) ? 0 : 1;
        chip->rhy          = v & 0x3F;
    }
}

// Resampled generation (produces output at any rate by averaging native samples)
// For DOOM we call OPL3_Generate directly and do our own resampling
void OPL3_GenerateResampled(opl3_chip *chip, int16_t *buf)
{
    OPL3_Generate(chip, buf);
}
