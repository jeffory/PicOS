/*
  DOS86 — 8253/8254 Programmable Interval Timer (PIT) emulation.

  Mode register (port 0x43) layout:
    bits 7-6: channel select (00=ch0, 01=ch1, 10=ch2, 11=read-back)
    bits 5-4: access mode  (00=latch, 01=LSB only, 10=MSB only, 11=LSB then MSB)
    bits 3-1: operating mode (0=mode0 .. 5=mode5)
    bit  0:   BCD (0=binary)  — we only support binary

  We implement Mode 2 (rate generator) and Mode 3 (square wave generator),
  which are the only modes used by IBM PC BIOS/DOS.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#include "i8253.h"
#include "i8259.h"
#include <stdint.h>

/* --- Per-channel state --------------------------------------------------- */
typedef struct {
    uint16_t reload;      /* value written via port (reload value)            */
    uint16_t count;       /* current down-counter                             */
    uint16_t latch;       /* latched count for read-back                      */
    uint8_t  access;      /* access mode: 1=LSB, 2=MSB, 3=LSB+MSB            */
    uint8_t  mode;        /* operating mode 0-5                               */
    uint8_t  write_phase; /* 0=next byte is LSB, 1=next byte is MSB (mode 3) */
    uint8_t  read_phase;  /* 0=next read is LSB, 1=next read is MSB          */
    uint8_t  latched;     /* 1 if count is latched (OLC command)             */
    uint8_t  active;      /* 1 after first full reload value written          */
    uint16_t write_buf;   /* partial 16-bit value during two-byte write       */
} pit_channel_t;

static pit_channel_t s_ch[3];

/* Fractional tick accumulator (to handle non-integer counts across calls) */
static uint32_t s_ch0_frac;

/* --- Reset --------------------------------------------------------------- */

void i8253_reset(void)
{
    for (int i = 0; i < 3; i++) {
        s_ch[i].reload      = 0;      /* 0 means 65536 on real hardware */
        s_ch[i].count       = 0;
        s_ch[i].latch       = 0;
        s_ch[i].access      = 3;      /* LSB+MSB default                */
        s_ch[i].mode        = 3;      /* square wave default            */
        s_ch[i].write_phase = 0;
        s_ch[i].read_phase  = 0;
        s_ch[i].latched     = 0;
        s_ch[i].active      = 0;
        s_ch[i].write_buf   = 0;
    }
    s_ch0_frac = 0;
}

/* --- Tick ---------------------------------------------------------------- */

void i8253_tick(uint32_t ticks)
{
    pit_channel_t *ch = &s_ch[0];
    if (!ch->active) return;

    /* Use reload value; 0 means 65536. */
    uint32_t period = ch->reload ? ch->reload : 65536u;

    /* Accumulate ticks with fractional carry. */
    s_ch0_frac += ticks;
    while (s_ch0_frac >= period) {
        s_ch0_frac -= period;
        /* Channel 0 wrapped → fire IRQ0 */
        i8259_irq(0);
    }

    /* Update running count for read-back accuracy (approximate). */
    ch->count = (uint16_t)(period - (s_ch0_frac % period));
}

/* --- Speaker frequency --------------------------------------------------- */

uint32_t i8253_get_speaker_freq(void)
{
    pit_channel_t *ch = &s_ch[2];
    if (!ch->active) return 0;
    uint32_t reload = ch->reload ? ch->reload : 65536u;
    return PIT_CLOCK_HZ / reload;
}

/* --- Port I/O ------------------------------------------------------------ */

uint8_t i8253_port_read(uint16_t port)
{
    if (port == 0x43) {
        /* Mode register is write-only on real 8253; return 0xFF. */
        return 0xFF;
    }

    int ch_idx = port - 0x40;  /* 0, 1, or 2 */
    if (ch_idx < 0 || ch_idx > 2) return 0xFF;

    pit_channel_t *ch = &s_ch[ch_idx];
    uint16_t val = ch->latched ? ch->latch : ch->count;

    if (ch->access == 1) {
        /* LSB only */
        if (ch->latched) ch->latched = 0;
        return (uint8_t)(val & 0xFF);
    } else if (ch->access == 2) {
        /* MSB only */
        if (ch->latched) ch->latched = 0;
        return (uint8_t)(val >> 8);
    } else {
        /* LSB+MSB — two reads */
        if (ch->read_phase == 0) {
            ch->read_phase = 1;
            return (uint8_t)(val & 0xFF);
        } else {
            ch->read_phase = 0;
            if (ch->latched) ch->latched = 0;
            return (uint8_t)(val >> 8);
        }
    }
}

void i8253_port_write(uint16_t port, uint8_t val)
{
    if (port == 0x43) {
        /* Mode/Command register */
        uint8_t ch_idx = (val >> 6) & 0x03;
        if (ch_idx == 3) {
            /* Read-back command (8254 only) — ignore on 8253 emulation */
            return;
        }
        uint8_t access = (val >> 4) & 0x03;
        uint8_t mode   = (val >> 1) & 0x07;
        /* Mode values 4-7 on older docs; clamp to 0-5 */
        if (mode > 5) mode = mode & 0x03;

        pit_channel_t *ch = &s_ch[ch_idx];

        if (access == 0) {
            /* Counter latch command — latch current count for reading */
            ch->latch   = ch->count;
            ch->latched = 1;
        } else {
            ch->access      = access;
            ch->mode        = mode;
            ch->write_phase = 0;
            ch->read_phase  = 0;
            ch->latched     = 0;
            ch->active      = 0; /* wait for new reload value */
            ch->write_buf   = 0;
        }
        return;
    }

    /* Data port for channel 0/1/2 */
    int ch_idx = port - 0x40;
    if (ch_idx < 0 || ch_idx > 2) return;
    pit_channel_t *ch = &s_ch[ch_idx];

    if (ch->access == 1) {
        /* LSB only */
        ch->reload = (ch->reload & 0xFF00u) | val;
        ch->count  = ch->reload;
        ch->active = 1;
    } else if (ch->access == 2) {
        /* MSB only */
        ch->reload = (ch->reload & 0x00FFu) | ((uint16_t)val << 8);
        ch->count  = ch->reload;
        ch->active = 1;
    } else {
        /* LSB+MSB — two writes */
        if (ch->write_phase == 0) {
            ch->write_buf   = val;          /* store LSB */
            ch->write_phase = 1;
        } else {
            ch->reload = ch->write_buf | ((uint16_t)val << 8);
            ch->count  = ch->reload;
            ch->active = 1;
            ch->write_phase = 0;
            if (ch_idx == 0) s_ch0_frac = 0; /* restart accumulator */
        }
    }
}
