/*
  DOS86 — 8259A Programmable Interrupt Controller (PIC) emulation.
  Single master PIC, IRQ0–IRQ7.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#ifndef FAKE86_I8259_H
#define FAKE86_I8259_H

#include <stdint.h>

/* Reset the PIC to its power-on state (all IRQs masked, vector base 0x08). */
void i8259_reset(void);

/* Assert an IRQ line (0-7).  Sets the corresponding bit in IRR if not masked. */
void i8259_irq(uint8_t irq_num);

/* Check for the highest-priority pending unmasked interrupt.
   Clears the IRR bit, sets the ISR bit, and returns the interrupt vector.
   Returns -1 if no interrupt is pending. */
int  i8259_poll(void);

/* Port I/O handlers — wired by ports.c in Task 4. */
uint8_t  i8259_port_read(uint16_t port);
void     i8259_port_write(uint16_t port, uint8_t val);

#endif /* FAKE86_I8259_H */
