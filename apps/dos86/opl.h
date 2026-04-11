/*
  DOS86 — OPL2 (AdLib) FM synthesis interface.

  Wraps the OPL2 emulation core in opl2/. DOS programs write to I/O ports
  0x388 (register select) and 0x389 (data write); reads from 0x388 return
  the status byte (timer flags).

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#ifndef DOS86_OPL_H
#define DOS86_OPL_H

#include <stdint.h>

/* Initialize the OPL2 chip at the given output sample rate. */
void opl_init(uint32_t sample_rate);

/* Write to an OPL2 I/O port (0x388 = register select, 0x389 = data). */
void opl_write(uint16_t port, uint8_t val);

/* Read from an OPL2 I/O port (0x388 = status register). */
uint8_t opl_read(uint16_t port);

/* Generate num_samples of mono int16_t PCM into buf. */
void opl_generate(int16_t *buf, int num_samples);

/* Shut down the OPL2 emulator. */
void opl_shutdown(void);

#endif /* DOS86_OPL_H */
