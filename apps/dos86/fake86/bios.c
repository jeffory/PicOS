/*
  DOS86 — BIOS interrupt handlers: INT 16h (keyboard), INT 1Ah (time).

  INT 16h — Keyboard BIOS Services
    AH=00h  Wait for keypress (non-blocking retry: rewinds IP if no key)
    AH=01h  Check for keypress (non-blocking peek)
    AH=02h  Get shift flags

  INT 1Ah — Time Services
    AH=00h  Get system timer ticks (18.2065 Hz)
    AH=02h  Get RTC time (BCD)
    AH=04h  Get RTC date (BCD, fixed: 2026-01-01)
*/

#include "bios.h"
#include "cpu.h"
#include "../backend.h"

/* ---- Scancode → ASCII translation ---- */

/*
 * AT Set 1 make-code to ASCII table.
 * Index = scancode (0x00..0x3F range covered).
 * Two entries per scancode: [unshifted, shifted].
 * 0x00 = no ASCII (function key or unmapped).
 */
static const uint8_t s_sc_ascii[0x58][2] = {
    /* 0x00 */ { 0x00, 0x00 },
    /* 0x01 */ { 0x1B, 0x1B },  /* Esc */
    /* 0x02 */ { '1',  '!' },
    /* 0x03 */ { '2',  '@' },
    /* 0x04 */ { '3',  '#' },
    /* 0x05 */ { '4',  '$' },
    /* 0x06 */ { '5',  '%' },
    /* 0x07 */ { '6',  '^' },
    /* 0x08 */ { '7',  '&' },
    /* 0x09 */ { '8',  '*' },
    /* 0x0A */ { '9',  '(' },
    /* 0x0B */ { '0',  ')' },
    /* 0x0C */ { '-',  '_' },
    /* 0x0D */ { '=',  '+' },
    /* 0x0E */ { 0x08, 0x08 },  /* Backspace */
    /* 0x0F */ { 0x09, 0x09 },  /* Tab */
    /* 0x10 */ { 'q',  'Q' },
    /* 0x11 */ { 'w',  'W' },
    /* 0x12 */ { 'e',  'E' },
    /* 0x13 */ { 'r',  'R' },
    /* 0x14 */ { 't',  'T' },
    /* 0x15 */ { 'y',  'Y' },
    /* 0x16 */ { 'u',  'U' },
    /* 0x17 */ { 'i',  'I' },
    /* 0x18 */ { 'o',  'O' },
    /* 0x19 */ { 'p',  'P' },
    /* 0x1A */ { '[',  '{' },
    /* 0x1B */ { ']',  '}' },
    /* 0x1C */ { 0x0D, 0x0D },  /* Enter */
    /* 0x1D */ { 0x00, 0x00 },  /* Ctrl */
    /* 0x1E */ { 'a',  'A' },
    /* 0x1F */ { 's',  'S' },
    /* 0x20 */ { 'd',  'D' },
    /* 0x21 */ { 'f',  'F' },
    /* 0x22 */ { 'g',  'G' },
    /* 0x23 */ { 'h',  'H' },
    /* 0x24 */ { 'j',  'J' },
    /* 0x25 */ { 'k',  'K' },
    /* 0x26 */ { 'l',  'L' },
    /* 0x27 */ { ';',  ':' },
    /* 0x28 */ { '\'', '"' },
    /* 0x29 */ { '`',  '~' },
    /* 0x2A */ { 0x00, 0x00 },  /* Left Shift */
    /* 0x2B */ { '\\', '|' },
    /* 0x2C */ { 'z',  'Z' },
    /* 0x2D */ { 'x',  'X' },
    /* 0x2E */ { 'c',  'C' },
    /* 0x2F */ { 'v',  'V' },
    /* 0x30 */ { 'b',  'B' },
    /* 0x31 */ { 'n',  'N' },
    /* 0x32 */ { 'm',  'M' },
    /* 0x33 */ { ',',  '<' },
    /* 0x34 */ { '.',  '>' },
    /* 0x35 */ { '/',  '?' },
    /* 0x36 */ { 0x00, 0x00 },  /* Right Shift */
    /* 0x37 */ { '*',  '*' },   /* Keypad * */
    /* 0x38 */ { 0x00, 0x00 },  /* Left Alt */
    /* 0x39 */ { ' ',  ' ' },   /* Space */
    /* 0x3A */ { 0x00, 0x00 },  /* Caps Lock */
    /* 0x3B */ { 0x00, 0x00 },  /* F1 */
    /* 0x3C */ { 0x00, 0x00 },  /* F2 */
    /* 0x3D */ { 0x00, 0x00 },  /* F3 */
    /* 0x3E */ { 0x00, 0x00 },  /* F4 */
    /* 0x3F */ { 0x00, 0x00 },  /* F5 */
    /* 0x40 */ { 0x00, 0x00 },  /* F6 */
    /* 0x41 */ { 0x00, 0x00 },  /* F7 */
    /* 0x42 */ { 0x00, 0x00 },  /* F8 */
    /* 0x43 */ { 0x00, 0x00 },  /* F9 */
    /* 0x44 */ { 0x00, 0x00 },  /* F10 */
    /* 0x45 */ { 0x00, 0x00 },  /* Num Lock */
    /* 0x46 */ { 0x00, 0x00 },  /* Scroll Lock */
    /* 0x47 */ { 0x00, 0x00 },  /* Keypad 7 / Home */
    /* 0x48 */ { 0x00, 0x00 },  /* Keypad 8 / Up */
    /* 0x49 */ { 0x00, 0x00 },  /* Keypad 9 / PgUp */
    /* 0x4A */ { '-',  '-' },   /* Keypad - */
    /* 0x4B */ { 0x00, 0x00 },  /* Keypad 4 / Left */
    /* 0x4C */ { 0x00, 0x00 },  /* Keypad 5 */
    /* 0x4D */ { 0x00, 0x00 },  /* Keypad 6 / Right */
    /* 0x4E */ { '+',  '+' },   /* Keypad + */
    /* 0x4F */ { 0x00, 0x00 },  /* Keypad 1 / End */
    /* 0x50 */ { 0x00, 0x00 },  /* Keypad 2 / Down */
    /* 0x51 */ { 0x00, 0x00 },  /* Keypad 3 / PgDn */
    /* 0x52 */ { 0x00, 0x00 },  /* Keypad 0 / Ins */
    /* 0x53 */ { 0x7F, 0x7F },  /* Keypad . / Del */
    /* 0x54 */ { 0x00, 0x00 },
    /* 0x55 */ { 0x00, 0x00 },
    /* 0x56 */ { 0x00, 0x00 },
    /* 0x57 */ { 0x00, 0x00 },  /* F11 */
};

/* Track shift/ctrl/alt state from make/break codes in the kb buffer */
static uint8_t s_shift_flags = 0;  /* bit0=rshift, bit1=lshift, bit2=ctrl, bit3=alt */

/* Convert scancode + current shift state to ASCII */
static uint8_t scancode_to_ascii(uint8_t sc) {
    int shifted = (s_shift_flags & 0x03) ? 1 : 0;  /* left or right shift */
    if (sc >= 0x58) return 0;
    return s_sc_ascii[sc][shifted];
}

/* Update modifier tracking from a scancode (make or break) */
static void update_modifiers(uint8_t sc) {
    int is_break = (sc & 0x80) != 0;
    uint8_t make = sc & 0x7F;

    if (make == 0x2A) {  /* Left Shift */
        if (is_break) s_shift_flags &= ~0x02;
        else          s_shift_flags |=  0x02;
    } else if (make == 0x36) {  /* Right Shift */
        if (is_break) s_shift_flags &= ~0x01;
        else          s_shift_flags |=  0x01;
    } else if (make == 0x1D) {  /* Ctrl */
        if (is_break) s_shift_flags &= ~0x04;
        else          s_shift_flags |=  0x04;
    } else if (make == 0x38) {  /* Alt */
        if (is_break) s_shift_flags &= ~0x08;
        else          s_shift_flags |=  0x08;
    }
}

/*
 * Consume scancodes from the backend buffer, updating modifier state,
 * until we find a make code that produces an ASCII character (or a
 * non-ASCII make code for AH=00h extended keys), or the buffer is empty.
 *
 * Returns the make scancode (0x00..0x7F) if a key is ready, or 0xFF if
 * the buffer is empty after draining modifiers/break codes.
 */
static uint8_t kb_drain_to_key(void) {
    while (backend_kb_available()) {
        uint8_t sc = backend_kb_read();
        update_modifiers(sc);

        /* Skip break codes (they only update modifier state) */
        if (sc & 0x80) continue;

        /* It's a make code — return it */
        return sc;
    }
    return 0xFF;  /* nothing ready */
}

/* ---- INT 16h — Keyboard BIOS Services ---- */

void bios_int16h(void) {
    uint8_t ah = regs.byteregs[regah];

    switch (ah) {
        case 0x00: {
            /*
             * AH=00h — Wait for keypress.
             *
             * Non-blocking retry strategy: if no key is in the buffer, rewind
             * IP back to the INT instruction (2 bytes: opcode 0xCD + 0x16) so
             * that exec86() re-executes INT 16h on the next iteration, giving
             * the main loop a chance to call backend_pump_keyboard() between
             * retries.  This avoids blocking inside the emulator core.
             */
            backend_pump_keyboard();
            uint8_t sc = kb_drain_to_key();
            if (sc == 0xFF) {
                /* No key yet — back up IP to re-execute INT 16h */
                ip = saveip;
                return;
            }
            regs.byteregs[regah] = sc;
            regs.byteregs[regal] = scancode_to_ascii(sc);
            break;
        }

        case 0x01: {
            /*
             * AH=01h — Check for keypress (non-blocking peek).
             * Sets ZF=0 and puts scancode in AH if key available.
             * Sets ZF=1 if no key.
             */
            backend_pump_keyboard();
            if (backend_kb_available()) {
                /* Peek: read, remember, and push back isn't possible without
                 * a peek API.  We drain to the first make-code and put it
                 * back by re-pushing it to the front.  Since the ring buffer
                 * only supports push-to-tail, we peek by reading then
                 * immediately re-injecting via backend_kb_push.
                 * Note: this loses modifier-only codes already in the queue,
                 * but that's acceptable for DOS compatibility. */
                uint8_t sc = kb_drain_to_key();
                if (sc != 0xFF) {
                    /* Re-inject so AH=00h can consume it later */
                    backend_kb_push(sc);
                    regs.byteregs[regah] = sc;
                    regs.byteregs[regal] = scancode_to_ascii(sc);
                    zf = 0;  /* Key is available */
                } else {
                    zf = 1;  /* No key */
                }
            } else {
                zf = 1;  /* No key */
            }
            break;
        }

        case 0x02:
            /* AH=02h — Get shift flags */
            regs.byteregs[regal] = s_shift_flags;
            break;

        default:
            /* Unsupported subfunction — ignore */
            break;
    }
}

/* ---- Helper: convert uint8 to BCD ---- */
static uint8_t to_bcd(uint8_t val) {
    return (uint8_t)(((val / 10) << 4) | (val % 10));
}

/* ---- INT 1Ah — Time Services ---- */

void bios_int1ah(void) {
    uint8_t ah = regs.byteregs[regah];

    switch (ah) {
        case 0x00: {
            /*
             * AH=00h — Get system timer tick count.
             * DOS expects 18.2065 ticks per second since boot.
             * ticks = ms * 182065 / 10000000
             */
            uint32_t ms = backend_get_ms();
            /* Use 64-bit intermediate to avoid overflow */
            uint32_t ticks = (uint32_t)(((uint64_t)ms * 182065ULL) / 10000000ULL);
            regs.wordregs[regcx] = (uint16_t)(ticks >> 16);   /* High word */
            regs.wordregs[regdx] = (uint16_t)(ticks & 0xFFFF); /* Low word */
            regs.byteregs[regal] = 0;  /* Midnight flag = 0 */
            break;
        }

        case 0x02: {
            /*
             * AH=02h — Get RTC time (BCD).
             * Compute H:M:S from milliseconds since boot.
             */
            uint32_t ms = backend_get_ms();
            uint32_t total_sec = ms / 1000;
            uint8_t  hours   = (uint8_t)((total_sec / 3600) % 24);
            uint8_t  minutes = (uint8_t)((total_sec / 60) % 60);
            uint8_t  seconds = (uint8_t)(total_sec % 60);

            regs.byteregs[regch] = to_bcd(hours);
            regs.byteregs[regcl] = to_bcd(minutes);
            regs.byteregs[regdh] = to_bcd(seconds);
            cf = 0;  /* Success */
            break;
        }

        case 0x04: {
            /*
             * AH=04h — Get RTC date (BCD).
             * Return a fixed date: 2026-01-01.
             */
            regs.byteregs[regch] = 0x20;  /* Century: 20 (BCD) */
            regs.byteregs[regcl] = 0x26;  /* Year:    26 (BCD) */
            regs.byteregs[regdh] = 0x01;  /* Month:   01 (BCD) */
            regs.byteregs[regdl] = 0x01;  /* Day:     01 (BCD) */
            cf = 0;  /* Success */
            break;
        }

        default:
            /* Unsupported subfunction */
            cf = 1;
            break;
    }
}
