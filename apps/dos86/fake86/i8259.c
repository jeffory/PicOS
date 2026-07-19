/*
  DOS86 — 8259A Programmable Interrupt Controller (PIC) emulation.
  Single master PIC, IRQ0–IRQ7.

  Real 8259 reference: Intel 8259A datasheet, 1988.
  ICW1 → ICW2 → (ICW3 skipped, single chip) → ICW4 → ready.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#include "i8259.h"
#include <stdint.h>

/* --- PIC state ----------------------------------------------------------- */
static struct {
    uint8_t irr;       /* Interrupt Request Register  — bits set by i8259_irq() */
    uint8_t isr;       /* In-Service Register         — bit set while servicing  */
    uint8_t imr;       /* Interrupt Mask Register     — 1 = masked (disabled)    */
    uint8_t vector_base; /* ICW2: base interrupt vector (added to irq 0-7)       */

    /* ICW initialization state machine */
    uint8_t icw_step;  /* 0 = ready, 1 = expect ICW2, 2 = expect ICW4           */
    uint8_t icw1;      /* saved ICW1 flags                                        */

    uint8_t ocw3;      /* current OCW3 register (selects IRR vs ISR on read)     */
} s_pic;

/* --- Public API ---------------------------------------------------------- */

void i8259_reset(void)
{
    s_pic.irr         = 0x00;
    s_pic.isr         = 0x00;
    s_pic.imr         = 0xFF; /* all IRQs masked after reset */
    s_pic.vector_base = 0x08; /* IBM PC BIOS default: IRQ0 → INT 08h */
    s_pic.icw_step    = 0;
    s_pic.icw1        = 0;
    s_pic.ocw3        = 0x02; /* default: read IRR on port 0x20 */
}

void i8259_irq(uint8_t irq_num)
{
    if (irq_num > 7) return;
    /* Set IRR bit; masking is checked at dispatch time in i8259_poll(). */
    s_pic.irr |= (uint8_t)(1u << irq_num);
}

int i8259_poll(void)
{
    /* Find highest-priority (lowest bit number) unmasked pending interrupt. */
    uint8_t pending = s_pic.irr & ~s_pic.imr;
    if (!pending) return -1;

    /* Priority: IRQ0 > IRQ1 > ... > IRQ7 */
    for (int i = 0; i < 8; i++) {
        if (pending & (1u << i)) {
            s_pic.irr &= ~(uint8_t)(1u << i); /* acknowledge from IRR */
            s_pic.isr |=  (uint8_t)(1u << i); /* mark in-service       */
            return s_pic.vector_base + i;
        }
    }
    return -1;
}

/* --- Port I/O ------------------------------------------------------------ */
/*
 * Port 0x20: command (write) / status (read)
 * Port 0x21: data / IMR
 *
 * Write to 0x20:
 *   Bit 4 set → ICW1 (initialization command word 1)
 *   Bit 3 set + bit 4 clear → OCW3
 *   Otherwise → OCW2 (e.g. End-Of-Interrupt = 0x20)
 */

uint8_t i8259_port_read(uint16_t port)
{
    if (port == 0x20) {
        /* Return IRR or ISR depending on OCW3 read-register select bit. */
        if (s_pic.ocw3 & 0x01)
            return s_pic.isr;
        else
            return s_pic.irr;
    }
    /* port == 0x21 */
    return s_pic.imr;
}

void i8259_port_write(uint16_t port, uint8_t val)
{
    if (port == 0x20) {
        if (val & 0x10) {
            /* ICW1 — start initialization sequence */
            s_pic.icw1     = val;
            s_pic.imr      = 0x00;  /* clear IMR during init */
            s_pic.isr      = 0x00;
            s_pic.irr      = 0x00;
            s_pic.ocw3     = 0x02;
            s_pic.icw_step = 1;     /* next write to 0x21 is ICW2 */
        } else if (val & 0x08) {
            /* OCW3 */
            s_pic.ocw3 = val;
        } else {
            /* OCW2 — End-Of-Interrupt and priority commands */
            uint8_t cmd = (val >> 5) & 0x07;
            if (cmd == 0x01) {
                /* Non-specific EOI: clear lowest ISR bit */
                for (int i = 0; i < 8; i++) {
                    if (s_pic.isr & (1u << i)) {
                        s_pic.isr &= ~(uint8_t)(1u << i);
                        break;
                    }
                }
            } else if (cmd == 0x03) {
                /* Specific EOI: clear the IRQ level in bits 0-2 */
                s_pic.isr &= ~(uint8_t)(1u << (val & 0x07));
            }
            /* cmd 0x00 = rotate in auto-EOI (not commonly needed, ignore) */
        }
    } else {
        /* port == 0x21 */
        if (s_pic.icw_step == 1) {
            /* ICW2: interrupt vector base */
            s_pic.vector_base = val & 0xF8; /* low 3 bits must be 0 */
            /* Check if ICW4 is needed (ICW1 bit 0) */
            if (s_pic.icw1 & 0x01) {
                s_pic.icw_step = 2; /* expect ICW4 next */
            } else {
                s_pic.icw_step = 0; /* done */
            }
        } else if (s_pic.icw_step == 2) {
            /* ICW4: 8086/88 mode bit etc. — accept and ignore specifics */
            s_pic.icw_step = 0; /* ready */
        } else {
            /* OCW1: Interrupt Mask Register */
            s_pic.imr = val;
        }
    }
}
