/*
  DOS86 — I/O port dispatcher.
  Routes port reads/writes to the correct peripheral emulator.
*/

#include "ports.h"
#include "i8259.h"
#include "i8253.h"
#include "video.h"
#include "../backend.h"
#include "../speaker.h"
#include <stdint.h>

/* Port 0x61: keyboard/speaker control register */
static uint8_t s_port61 = 0;

void ports_reset(void) {
    s_port61 = 0;
}

uint8_t portin8(uint16_t port) {
    /* 8259 PIC: ports 0x20-0x21 */
    if (port == 0x20 || port == 0x21) {
        return i8259_port_read(port);
    }

    /* 8253 PIT: ports 0x40-0x43 */
    if (port >= 0x40 && port <= 0x43) {
        return i8253_port_read(port);
    }

    /* Keyboard data register */
    if (port == 0x60) {
        return backend_kb_read();
    }

    /* Keyboard/speaker control register */
    if (port == 0x61) {
        return s_port61;
    }

    /* Keyboard status register */
    if (port == 0x64) {
        return backend_kb_available() ? 0x01 : 0x00;
    }

    /* VGA/CGA video registers: 0x3B0-0x3DA */
    if (port >= 0x3B0 && port <= 0x3DA) {
        return video_portin(port);
    }

    /* OPL2 (AdLib): stubbed */
    if (port == 0x388 || port == 0x389) {
        return 0xFF;
    }

    /* Default: return port shadow RAM value */
    return g_portram[port];
}

uint16_t portin16(uint16_t port) {
    return (uint16_t)portin8(port) | ((uint16_t)portin8((uint16_t)(port + 1)) << 8);
}

void portout8(uint16_t port, uint8_t val) {
    /* 8259 PIC: ports 0x20-0x21 */
    if (port == 0x20 || port == 0x21) {
        i8259_port_write(port, val);
        return;
    }

    /* 8253 PIT: ports 0x40-0x43 */
    if (port >= 0x40 && port <= 0x43) {
        i8253_port_write(port, val);
        return;
    }

    /* Keyboard/speaker control register */
    if (port == 0x61) {
        s_port61 = val;
        /* Both bit 0 (PIT ch2 gate) and bit 1 (speaker out) must be set for sound */
        speaker_set_gate((val & 0x03) == 0x03);
        return;
    }

    /* VGA/CGA video registers: 0x3B0-0x3DA */
    if (port >= 0x3B0 && port <= 0x3DA) {
        video_portout(port, val);
        return;
    }

    /* OPL2 (AdLib): stubbed no-op */
    if (port == 0x388 || port == 0x389) {
        return;
    }

    /* Default: store in port shadow RAM */
    g_portram[port] = val;
}

void portout16(uint16_t port, uint16_t val) {
    portout8(port, (uint8_t)val);
    portout8((uint16_t)(port + 1), (uint8_t)(val >> 8));
}

/* Aliases used by cpu.c */
uint8_t portin(uint16_t port) { return portin8(port); }
void    portout(uint16_t port, uint8_t val) { portout8(port, val); }
