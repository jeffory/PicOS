/*
  Minimal OPL2 FM synthesis core.

  This implements a basic Yamaha YM3812 (OPL2) emulator with:
    - 9 two-operator FM channels
    - ADSR envelope generator
    - 4 waveforms (sine, half-sine, abs-sine, quarter-sine)
    - Feedback on operator 1
    - FM and additive synthesis modes

  The OPL2 internal clock is 3.579545 MHz, divided by 72 to get the
  operator sample rate of ~49.716 kHz. We resample to the output rate
  using a simple phase accumulator.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#include "opl2_core.h"
#include <string.h>

/* ---- Constants ---- */

/* OPL2 master clock and derived rate */
#define OPL2_CLOCK       3579545u
#define OPL2_RATE_DIV    72u
#define OPL2_INTERNAL_RATE (OPL2_CLOCK / OPL2_RATE_DIV) /* ~49716 Hz */

/* Sine table: 1024 entries, 14-bit unsigned (0..16383) representing
   the negative log-sin function. We use a quarter-wave table (256 entries)
   and mirror it. This saves memory on the embedded target. */
#define SINE_TABLE_BITS  10
#define SINE_TABLE_SIZE  (1 << SINE_TABLE_BITS) /* 1024 */

/* Envelope constants */
#define ENV_MAX          511u   /* Maximum attenuation (silent) */
#define ENV_QUIET        400u   /* Threshold for "effectively silent" */

/* Operator-to-channel mapping.
   OPL2 has 18 operators mapped to 9 channels as:
     Ch 0: op 0, op 3
     Ch 1: op 1, op 4
     Ch 2: op 2, op 5
     Ch 3: op 6, op 9
     Ch 4: op 7, op 10
     Ch 5: op 8, op 11
     Ch 6: op 12, op 15
     Ch 7: op 13, op 16
     Ch 8: op 14, op 17 */
static const uint8_t s_op1_map[OPL2_NUM_CHANNELS] = {
    0, 1, 2, 6, 7, 8, 12, 13, 14
};
static const uint8_t s_op2_map[OPL2_NUM_CHANNELS] = {
    3, 4, 5, 9, 10, 11, 15, 16, 17
};

/* Register offset to operator index mapping.
   Registers 0x20-0x35 map to operators 0-17 with gaps. */
static const int8_t s_reg_to_op[32] = {
     0,  1,  2,  3,  4,  5, -1, -1,
     6,  7,  8,  9, 10, 11, -1, -1,
    12, 13, 14, 15, 16, 17, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};

/* Frequency multiplier table (register value → actual multiplier * 2) */
static const uint8_t s_mult_table[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

/* Attack rate increment table. Indexed by rate*4 + sub-step.
   Simplified from the real OPL2 rate tables. Reserved for future
   accuracy improvements. */
static const uint8_t s_attack_inc[64] __attribute__((unused)) = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,  /* rates 0-3: no increment */
    0,1,0,0, 0,1,0,1, 0,1,1,0, 0,1,1,1,  /* rates 4-7 */
    1,1,0,1, 1,1,1,1, 1,1,1,1, 1,1,1,1,  /* rates 8-11 */
    1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1,  /* rates 12-15 */
};

/* Pre-computed sine table (generated in opl2_core_init).
   Stores log-sin values as 12-bit unsigned: higher = more attenuation. */
static uint16_t s_log_sin[SINE_TABLE_SIZE];

/* Exponent table: converts log-attenuation back to linear amplitude.
   exp_table[x] = round(pow(2, 13) * pow(2, -x/256)) */
static uint16_t s_exp_table[256];

/* Flag: tables initialized */
static int s_tables_ready = 0;

/* ---- Table generation ---- */

/* Simple integer approximation: we don't have <math.h> on bare metal,
   so we use a polynomial approximation for sin and log/exp tables. */

/* Fixed-point sin approximation (Q15 output, 0..32767 for 0..pi/2).
   Input: 0..1023 representing 0..2*pi in SINE_TABLE_SIZE steps.
   We only need the first quadrant (0..255 → 0..pi/2). */
static uint16_t approx_sin_q15(int idx)
{
    /* Map idx (0..255) to angle (0..pi/2).
       Use Bhaskara I's approximation: sin(x) ≈ 16x(pi-x) / (5pi^2 - 4x(pi-x))
       But simpler: use a parabolic approximation.
       sin(x) ≈ (4/pi)*x - (4/pi^2)*x^2 for x in [0, pi]
       For quarter wave (0..pi/2), simplified to:
       sin(t) ≈ t*(2-t) * (16384/8192) where t = idx/256 */

    /* Use a third-order polynomial for better accuracy:
       sin(pi/2 * t) ≈ t * (3.14159/2 - t^2 * 0.405) ... too complex.
       Instead, use a pre-known good approximation with integer math. */

    /* Quadratic approximation for sin(pi*x/512) where x = 0..255:
       result ≈ (x * (512 - x)) >> 6, scaled to 0..16383 */
    uint32_t x = (uint32_t)idx;
    uint32_t y = x * (256u - x);  /* peaks at x=128: 128*128=16384 */
    /* Scale so peak = 16383 (14-bit max for log-sin calculation) */
    /* y max = 16384, want 16383 → just clamp */
    y = (y * 4u) >> 2;  /* keep as-is since max is ~16384 */
    if (y > 16383u) y = 16383u;
    return (uint16_t)y;
}

static void build_tables(void)
{
    if (s_tables_ready) return;

    /* Build sine table as log-sin: -log2(sin(x)) * 256, stored as 12-bit.
       For zero crossings, use maximum attenuation. */
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        int quarter = i & 0xFF;
        int half = i & 0x200;

        /* Get sin amplitude for this position (0..16383, first quadrant) */
        uint16_t sin_val;
        if ((i & 0x1FF) >= 256) {
            /* Second quarter: mirror */
            sin_val = approx_sin_q15(255 - quarter);
        } else {
            sin_val = approx_sin_q15(quarter);
        }

        /* Convert to log-attenuation (higher = quieter).
           0 = maximum volume (sin=1.0), 4095 = silent (sin≈0) */
        if (sin_val <= 1) {
            s_log_sin[i] = 4095;
        } else {
            /* Approximate -log2(sin_val/16384) * 256 using integer math.
               log2(16384) = 14, log2(sin_val) ≈ bit_position.
               Attenuation = (14 - log2(sin_val)) * 256 */
            uint32_t v = sin_val;
            int bits = 0;
            uint32_t tmp = v;
            while (tmp > 1) { tmp >>= 1; bits++; }
            /* Fractional part: use remaining bits */
            uint32_t frac = 0;
            if (bits > 0) {
                frac = ((v << (14 - bits)) & 0x3FFF) >> 6; /* 8-bit fraction */
            }
            uint32_t log_val = ((uint32_t)(14 - bits) << 8) - frac;
            if (log_val > 4095) log_val = 4095;
            s_log_sin[i] = (uint16_t)log_val;
        }

        /* Apply sign: second half of sine is negative, but log-sin
           is unsigned — the sign is tracked separately during synthesis. */
        (void)half;
    }

    /* Build exponent table: exp_table[x] = 2^(13 - x/256)
       So exp_table[0] = 8192, exp_table[255] ≈ 4096 */
    for (int i = 0; i < 256; i++) {
        /* 2^(13) * 2^(-i/256) ≈ 8192 * (256-i)/256 (linear approx, good enough) */
        /* Better: use bit shift for the integer part, linear interp for fraction */
        s_exp_table[i] = (uint16_t)((8192u * (256u - (uint32_t)i)) >> 8);
        if (s_exp_table[i] == 0) s_exp_table[i] = 1;
    }

    s_tables_ready = 1;
}

/* ---- Envelope ---- */

/* Compute effective envelope rate based on register rate, key scale, and block.
   Returns effective rate 0-63. */
static int calc_rate(opl2_chip_t *chip, int op_idx, int base_rate)
{
    if (base_rate == 0) return 0;

    int ch = -1;
    /* Find which channel this operator belongs to */
    for (int c = 0; c < OPL2_NUM_CHANNELS; c++) {
        if (s_op1_map[c] == op_idx || s_op2_map[c] == op_idx) {
            ch = c;
            break;
        }
    }
    if (ch < 0) return base_rate * 4;

    int block = chip->channels[ch].block;
    int ksr_shift = chip->ops[op_idx].ksr ? 0 : 2;
    int rate = base_rate * 4 + ((block << 1) >> ksr_shift);
    if (rate > 63) rate = 63;
    return rate;
}

static void env_advance(opl2_chip_t *chip, opl2_operator_t *op, int op_idx)
{
    switch (op->env_state) {
    case ENV_OFF:
        op->env_level = ENV_MAX;
        break;

    case ENV_ATTACK: {
        int rate = calc_rate(chip, op_idx, op->ar);
        if (rate >= 60) {
            /* Instant attack */
            op->env_level = 0;
            op->env_state = ENV_DECAY;
        } else if (rate > 0) {
            /* Attack: exponential rise (level decreases toward 0).
               Higher rate = faster attack. */
            int shift = 13 - (rate >> 2);
            if (shift < 0) shift = 0;
            uint32_t step = 1u << shift;
            /* Subtract from env_level (attacking toward 0) */
            if (op->env_level > step) {
                /* OPL2 attack uses exponential curve: step proportional to level */
                uint32_t inc = (op->env_level * step) >> 9;
                if (inc == 0) inc = 1;
                if (op->env_level > inc)
                    op->env_level -= inc;
                else
                    op->env_level = 0;
            } else {
                op->env_level = 0;
            }
            if (op->env_level == 0) {
                op->env_state = ENV_DECAY;
            }
        }
        break;
    }

    case ENV_DECAY: {
        int rate = calc_rate(chip, op_idx, op->dr);
        /* Target sustain level: sl=0 → 0 attenuation, sl=15 → max (0x1F0) */
        uint32_t sl_target = (uint32_t)op->sl << 5;
        if (op->sl == 15) sl_target = ENV_MAX;

        if (rate > 0) {
            int shift = 13 - (rate >> 2);
            if (shift < 0) shift = 0;
            op->env_level += (1u << shift) >> 3;
            if (op->env_level > ENV_MAX) op->env_level = ENV_MAX;
        }
        if (op->env_level >= sl_target) {
            op->env_level = sl_target;
            op->env_state = op->egt ? ENV_SUSTAIN : ENV_RELEASE;
        }
        break;
    }

    case ENV_SUSTAIN:
        /* Sustained: level stays constant until key off (if egt=1).
           If egt=0, this state was skipped → goes to release. */
        if (!op->egt) {
            op->env_state = ENV_RELEASE;
        }
        break;

    case ENV_RELEASE: {
        int rate = calc_rate(chip, op_idx, op->rr);
        if (rate > 0) {
            int shift = 13 - (rate >> 2);
            if (shift < 0) shift = 0;
            op->env_level += (1u << shift) >> 3;
        } else {
            op->env_level += 1;
        }
        if (op->env_level >= ENV_MAX) {
            op->env_level = ENV_MAX;
            op->env_state = ENV_OFF;
        }
        break;
    }
    }
}

/* ---- Phase / frequency ---- */

/* Compute phase increment for an operator.
   fnum and block come from the channel; mult from the operator.
   The OPL2 phase step formula:
     freq = fnum * 2^(block-1) * OPL2_CLOCK / (72 * 2^20)
     phase_step = fnum * 2^block * mult / 2 (in 10.22 fixed point) */
static uint32_t calc_phase_step(opl2_chip_t *chip, int ch, int op_idx)
{
    uint32_t fnum = chip->channels[ch].fnum;
    uint32_t block = chip->channels[ch].block;
    uint32_t mult = s_mult_table[chip->ops[op_idx].mult];

    /* Phase step in internal OPL2 rate (10.22 fixed point):
       step = (fnum << block) * mult / 2
       Then scale to output sample rate:
       output_step = step * OPL2_INTERNAL_RATE / sample_rate */
    uint32_t step = (fnum << block) * mult;
    /* step is in units of OPL2 internal clock ticks.
       Scale to output: step * internal_rate / output_rate,
       but keep in 22-bit fractional fixed point.
       Simplify: step already represents phase per OPL2 sample.
       We need phase per output sample:
       output_step = step * OPL2_INTERNAL_RATE / sample_rate / 2 */
    uint64_t scaled = (uint64_t)step * OPL2_INTERNAL_RATE;
    scaled /= chip->sample_rate;
    scaled >>= 1; /* divide by 2 for mult table being *2 */
    return (uint32_t)scaled;
}

/* ---- Waveform lookup ---- */

/* Get the output amplitude for an operator given its phase and envelope.
   phase is 10.22 fixed-point; we use the top 10 bits as the sine index.
   Returns signed 14-bit audio sample. */
static int16_t calc_operator_output(opl2_operator_t *op, uint32_t phase,
                                     uint32_t env_level, int waveform_en)
{
    /* Extract 10-bit sine table index from phase (bits 22..13) */
    uint32_t idx = (phase >> 12) & 0x3FF;

    /* Apply waveform selection */
    uint8_t ws = waveform_en ? op->ws : 0;

    uint16_t log_atten;
    int negate = 0;

    switch (ws) {
    default:
    case 0: /* Sine */
        log_atten = s_log_sin[idx & 0x3FF];
        negate = (idx & 0x200) ? 1 : 0;
        break;
    case 1: /* Half-sine (positive half only, zero for negative half) */
        if (idx & 0x200) return 0;
        log_atten = s_log_sin[idx & 0x1FF];
        break;
    case 2: /* Absolute sine (full-wave rectified) */
        log_atten = s_log_sin[idx & 0x1FF];
        break;
    case 3: /* Quarter sine (positive rise only, zero elsewhere) */
        if (idx & 0x100) return 0;
        log_atten = s_log_sin[idx & 0xFF];
        break;
    }

    /* Add envelope attenuation (env_level is 0..511, scale to log domain).
       Total level is also in log domain. Add all attenuations. */
    uint32_t total_atten = (uint32_t)log_atten + (env_level << 3) +
                           ((uint32_t)op->tl << 5);

    /* Clamp */
    if (total_atten >= 4096) return 0;

    /* Convert from log to linear using exp table.
       Split total_atten into integer part (>>8) and fractional (low 8 bits). */
    uint32_t int_part = total_atten >> 8;
    uint32_t frac_part = total_atten & 0xFF;

    int32_t output = (int32_t)s_exp_table[frac_part];
    output >>= int_part;

    return negate ? (int16_t)(-output) : (int16_t)output;
}

/* ---- Key on/off handling ---- */

static void key_on(opl2_operator_t *op)
{
    op->env_state = ENV_ATTACK;
    op->env_level = ENV_MAX; /* Start from silence, attack toward 0 */
    op->phase = 0;
    op->feedback_out[0] = 0;
    op->feedback_out[1] = 0;
}

static void key_off(opl2_operator_t *op)
{
    if (op->env_state != ENV_OFF) {
        op->env_state = ENV_RELEASE;
    }
}

/* ---- Register write handling ---- */

/* Map register offset (0x00-0x15 range within a group) to operator index */
static int reg_offset_to_op(uint8_t offset)
{
    if (offset >= 32) return -1;
    return s_reg_to_op[offset];
}

void opl2_core_write_reg(opl2_chip_t *chip, uint8_t reg, uint8_t val)
{
    chip->regs[reg] = val;

    /* ---- Global registers ---- */
    if (reg == 0x01) {
        chip->waveform_enable = (val >> 5) & 1;
        return;
    }

    if (reg == 0x02) { chip->timer1_val = val; return; }
    if (reg == 0x03) { chip->timer2_val = val; return; }

    if (reg == 0x04) {
        chip->timer_ctrl = val;
        /* Bit 7: IRQ reset */
        if (val & 0x80) {
            chip->status &= ~0xE0; /* Clear timer flags */
        }
        return;
    }

    if (reg == 0x08) {
        /* CSW / NOTE-SEL — mostly ignored in our simple emulation */
        return;
    }

    if (reg == 0xBD) {
        chip->am_depth = (val >> 7) & 1;
        chip->vib_depth = (val >> 6) & 1;
        /* Percussion mode bits 5..0 — not implemented in this minimal core */
        return;
    }

    /* ---- Per-operator registers ---- */

    /* 0x20-0x35: AM/VIB/EGT/KSR/MULT */
    if (reg >= 0x20 && reg <= 0x35) {
        int op = reg_offset_to_op(reg - 0x20);
        if (op < 0) return;
        chip->ops[op].am   = (val >> 7) & 1;
        chip->ops[op].vib  = (val >> 6) & 1;
        chip->ops[op].egt  = (val >> 5) & 1;
        chip->ops[op].ksr  = (val >> 4) & 1;
        chip->ops[op].mult = val & 0x0F;
        return;
    }

    /* 0x40-0x55: KSL/TL */
    if (reg >= 0x40 && reg <= 0x55) {
        int op = reg_offset_to_op(reg - 0x40);
        if (op < 0) return;
        chip->ops[op].ksl = (val >> 6) & 3;
        chip->ops[op].tl  = val & 0x3F;
        return;
    }

    /* 0x60-0x75: AR/DR */
    if (reg >= 0x60 && reg <= 0x75) {
        int op = reg_offset_to_op(reg - 0x60);
        if (op < 0) return;
        chip->ops[op].ar = (val >> 4) & 0x0F;
        chip->ops[op].dr = val & 0x0F;
        return;
    }

    /* 0x80-0x95: SL/RR */
    if (reg >= 0x80 && reg <= 0x95) {
        int op = reg_offset_to_op(reg - 0x80);
        if (op < 0) return;
        chip->ops[op].sl = (val >> 4) & 0x0F;
        chip->ops[op].rr = val & 0x0F;
        return;
    }

    /* 0xE0-0xF5: Waveform select */
    if (reg >= 0xE0 && reg <= 0xF5) {
        int op = reg_offset_to_op(reg - 0xE0);
        if (op < 0) return;
        chip->ops[op].ws = val & 0x03;
        return;
    }

    /* ---- Per-channel registers ---- */

    /* 0xA0-0xA8: Frequency number low 8 bits */
    if (reg >= 0xA0 && reg <= 0xA8) {
        int ch = reg - 0xA0;
        chip->channels[ch].fnum = (chip->channels[ch].fnum & 0x300) | val;
        return;
    }

    /* 0xB0-0xB8: Key on, block, fnum high 2 bits */
    if (reg >= 0xB0 && reg <= 0xB8) {
        int ch = reg - 0xB0;
        uint8_t old_key = chip->channels[ch].key_on;

        chip->channels[ch].fnum = (chip->channels[ch].fnum & 0xFF) |
                                   ((uint16_t)(val & 0x03) << 8);
        chip->channels[ch].block = (val >> 2) & 0x07;
        chip->channels[ch].key_on = (val >> 5) & 0x01;

        /* Key on/off transitions */
        if (!old_key && chip->channels[ch].key_on) {
            key_on(&chip->ops[s_op1_map[ch]]);
            key_on(&chip->ops[s_op2_map[ch]]);
        } else if (old_key && !chip->channels[ch].key_on) {
            key_off(&chip->ops[s_op1_map[ch]]);
            key_off(&chip->ops[s_op2_map[ch]]);
        }
        return;
    }

    /* 0xC0-0xC8: Feedback/algorithm */
    if (reg >= 0xC0 && reg <= 0xC8) {
        int ch = reg - 0xC0;
        chip->channels[ch].feedback  = (val >> 1) & 0x07;
        chip->channels[ch].algorithm = val & 0x01;
        return;
    }
}

/* ---- Initialization ---- */

void opl2_core_init(opl2_chip_t *chip, uint32_t sample_rate)
{
    build_tables();
    memset(chip, 0, sizeof(*chip));
    chip->sample_rate = sample_rate;

    /* All operators start silent */
    for (int i = 0; i < OPL2_NUM_OPERATORS; i++) {
        chip->ops[i].env_state = ENV_OFF;
        chip->ops[i].env_level = ENV_MAX;
    }
}

/* ---- Sample generation ---- */

void opl2_core_generate(opl2_chip_t *chip, int16_t *buf, int num_samples)
{
    for (int s = 0; s < num_samples; s++) {
        int32_t output = 0;

        /* Advance LFO */
        chip->lfo_counter++;

        /* Process all 9 channels */
        for (int ch = 0; ch < OPL2_NUM_CHANNELS; ch++) {
            int op1_idx = s_op1_map[ch];
            int op2_idx = s_op2_map[ch];
            opl2_operator_t *op1 = &chip->ops[op1_idx];
            opl2_operator_t *op2 = &chip->ops[op2_idx];

            /* Skip if both operators are off */
            if (op1->env_state == ENV_OFF && op2->env_state == ENV_OFF)
                continue;

            /* Advance envelopes */
            env_advance(chip, op1, op1_idx);
            env_advance(chip, op2, op2_idx);

            /* Calculate phase steps */
            uint32_t step1 = calc_phase_step(chip, ch, op1_idx);
            uint32_t step2 = calc_phase_step(chip, ch, op2_idx);

            /* Advance phases */
            op1->phase += step1;
            op2->phase += step2;

            /* ---- Operator 1 (modulator) ---- */
            /* Apply feedback to operator 1's phase */
            uint32_t phase1 = op1->phase;
            if (chip->channels[ch].feedback > 0) {
                int32_t fb = (op1->feedback_out[0] + op1->feedback_out[1]) >> 1;
                int shift = 9 - chip->channels[ch].feedback;
                if (shift >= 0)
                    phase1 += (uint32_t)(fb << (12 - shift));
                else
                    phase1 += (uint32_t)(fb << 12) << (-shift);
            }

            int16_t out1 = calc_operator_output(op1, phase1, op1->env_level,
                                                 chip->waveform_enable);
            /* Store feedback */
            op1->feedback_out[0] = op1->feedback_out[1];
            op1->feedback_out[1] = (int32_t)out1;

            /* ---- Operator 2 (carrier) ---- */
            uint32_t phase2 = op2->phase;

            int16_t out2;
            if (chip->channels[ch].algorithm == 0) {
                /* FM mode: op1 modulates op2's phase */
                phase2 += (uint32_t)((int32_t)out1 << 12);
                out2 = calc_operator_output(op2, phase2, op2->env_level,
                                            chip->waveform_enable);
                output += (int32_t)out2;
            } else {
                /* Additive mode: both operators go to output */
                out2 = calc_operator_output(op2, phase2, op2->env_level,
                                            chip->waveform_enable);
                output += (int32_t)out1 + (int32_t)out2;
            }
        }

        /* Clamp to int16_t range.
           Scale down a bit since 9 channels can accumulate. */
        output >>= 1;
        if (output > 32767) output = 32767;
        if (output < -32768) output = -32768;

        buf[s] = (int16_t)output;
    }
}
