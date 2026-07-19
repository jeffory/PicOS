/*
  DOS86 — OPL2 (AdLib) FM synthesis wrapper.

  Bridges I/O port reads/writes (ports 0x388-0x389) to the OPL2 core
  emulator. Register select on port 0x388, data write on port 0x389.

  The register writes happen on Core 0 (during CPU emulation). Audio
  generation happens on Core 1 (in the audio callback). Since OPL register
  writes are byte-sized and the synthesis reads a consistent snapshot of
  the operator state, simple volatile access is sufficient — no lock needed.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#include "opl.h"
#include "opl2/opl2_core.h"

static opl2_chip_t s_chip;

void opl_init(uint32_t sample_rate)
{
    opl2_core_init(&s_chip, sample_rate);
}

void opl_write(uint16_t port, uint8_t val)
{
    if (port == 0x388) {
        /* Register select */
        s_chip.reg_select = val;
    } else if (port == 0x389) {
        /* Data write to selected register */
        opl2_core_write_reg(&s_chip, s_chip.reg_select, val);
    }
}

uint8_t opl_read(uint16_t port)
{
    if (port == 0x388) {
        /* Status register.
           Bit 7: IRQ (either timer flag set)
           Bit 6: Timer 1 overflow
           Bit 5: Timer 2 overflow
           Many games poll this for AdLib detection: they write timer values,
           start a timer, then read status looking for the overflow bit.
           We fake timer 1 always completing for AdLib detection to work. */

        /* Simple timer emulation: if timer 1 is running and not masked,
           set its flag after any write. Most detection routines do:
             1. Reset timers (write 0x60 to reg 4)
             2. Set timer 1 value (write 0xFF to reg 2)
             3. Start timer 1 (write 0x21 to reg 4)
             4. Wait ~80us then read status
           We set the flag immediately since our frame time >> 80us. */
        uint8_t status = s_chip.status;

        /* If timer 1 is started (bit 0 of timer_ctrl) and not masked (bit 6),
           set flag on next read */
        if ((s_chip.timer_ctrl & 0x01) && !(s_chip.timer_ctrl & 0x40)) {
            status |= 0xC0; /* Timer 1 flag + IRQ */
        }
        return status;
    }
    return 0xFF;
}

void opl_generate(int16_t *buf, int num_samples)
{
    opl2_core_generate(&s_chip, buf, num_samples);
}

void opl_shutdown(void)
{
    /* Nothing to free — all state is static */
}
