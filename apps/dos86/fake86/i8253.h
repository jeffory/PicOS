/*
  DOS86 — 8253/8254 Programmable Interval Timer (PIT) emulation.

  3 independent 16-bit down-counters:
    Channel 0 — System timer  → fires IRQ0 at ~18.2 Hz (reload 0 = 65536)
    Channel 1 — DRAM refresh  → stubbed, no action
    Channel 2 — PC Speaker    → drives square wave frequency

  Base clock: 1,193,182 Hz (PIT_CLOCK_HZ).

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#ifndef FAKE86_I8253_H
#define FAKE86_I8253_H

#include <stdint.h>

#define PIT_CLOCK_HZ  1193182u   /* PIT input clock frequency in Hz */

/* Reset all three PIT channels to power-on state. */
void i8253_reset(void);

/* Advance the PIT by 'ticks' PIT clock ticks.
   Fires IRQ0 (via i8259_irq(0)) each time channel 0 reaches zero. */
void i8253_tick(uint32_t ticks);

/* Return the current speaker tone frequency in Hz (channel 2).
   Returns 0 if channel 2 is not yet programmed / count is 0. */
uint32_t i8253_get_speaker_freq(void);

/* Port I/O handlers — wired by ports.c in Task 4.
 *   0x40: channel 0 data
 *   0x41: channel 1 data
 *   0x42: channel 2 data
 *   0x43: mode/command register (write-only on real hardware)
 */
uint8_t i8253_port_read(uint16_t port);
void    i8253_port_write(uint16_t port, uint8_t val);

#endif /* FAKE86_I8253_H */
