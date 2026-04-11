/*
  scancode.c — ASCII and BTN_* to AT Set 1 scancode translation tables.
*/

#include "scancode.h"
#include "os.h"

/* --------------------------------------------------------------------------
   ASCII → AT Set 1 make code table (128 entries, index = ASCII value).
   0x00 = unmapped.
   -------------------------------------------------------------------------- */

static const uint8_t s_ascii_scancode[128] = {
    /* 0x00 */ 0x00,  /* NUL */
    /* 0x01 */ 0x00,  /* SOH */
    /* 0x02 */ 0x00,  /* STX */
    /* 0x03 */ 0x00,  /* ETX */
    /* 0x04 */ 0x00,  /* EOT */
    /* 0x05 */ 0x00,  /* ENQ */
    /* 0x06 */ 0x00,  /* ACK */
    /* 0x07 */ 0x00,  /* BEL */
    /* 0x08 */ 0x0E,  /* BS  → Backspace */
    /* 0x09 */ 0x0F,  /* TAB → Tab */
    /* 0x0A */ 0x1C,  /* LF  → Enter */
    /* 0x0B */ 0x00,  /* VT  */
    /* 0x0C */ 0x00,  /* FF  */
    /* 0x0D */ 0x1C,  /* CR  → Enter */
    /* 0x0E */ 0x00,  /* SO  */
    /* 0x0F */ 0x00,  /* SI  */
    /* 0x10 */ 0x00,  /* DLE */
    /* 0x11 */ 0x00,  /* DC1 */
    /* 0x12 */ 0x00,  /* DC2 */
    /* 0x13 */ 0x00,  /* DC3 */
    /* 0x14 */ 0x00,  /* DC4 */
    /* 0x15 */ 0x00,  /* NAK */
    /* 0x16 */ 0x00,  /* SYN */
    /* 0x17 */ 0x00,  /* ETB */
    /* 0x18 */ 0x00,  /* CAN */
    /* 0x19 */ 0x00,  /* EM  */
    /* 0x1A */ 0x00,  /* SUB */
    /* 0x1B */ 0x01,  /* ESC → Escape */
    /* 0x1C */ 0x00,  /* FS  */
    /* 0x1D */ 0x00,  /* GS  */
    /* 0x1E */ 0x00,  /* RS  */
    /* 0x1F */ 0x00,  /* US  */
    /* 0x20 */ 0x39,  /* ' ' → Space */
    /* 0x21 */ 0x02,  /* '!' → 1 */
    /* 0x22 */ 0x28,  /* '"' → ' (shifted) */
    /* 0x23 */ 0x04,  /* '#' → 3 */
    /* 0x24 */ 0x05,  /* '$' → 4 */
    /* 0x25 */ 0x06,  /* '%' → 5 */
    /* 0x26 */ 0x08,  /* '&' → 7 */
    /* 0x27 */ 0x28,  /* ''' → ' */
    /* 0x28 */ 0x0A,  /* '(' → 9 */
    /* 0x29 */ 0x0B,  /* ')' → 0 */
    /* 0x2A */ 0x09,  /* '*' → 8 */
    /* 0x2B */ 0x0D,  /* '+' → = (shifted) */
    /* 0x2C */ 0x33,  /* ',' → , */
    /* 0x2D */ 0x0C,  /* '-' → - */
    /* 0x2E */ 0x34,  /* '.' → . */
    /* 0x2F */ 0x35,  /* '/' → / */
    /* 0x30 */ 0x0B,  /* '0' → 0 */
    /* 0x31 */ 0x02,  /* '1' → 1 */
    /* 0x32 */ 0x03,  /* '2' → 2 */
    /* 0x33 */ 0x04,  /* '3' → 3 */
    /* 0x34 */ 0x05,  /* '4' → 4 */
    /* 0x35 */ 0x06,  /* '5' → 5 */
    /* 0x36 */ 0x07,  /* '6' → 6 */
    /* 0x37 */ 0x08,  /* '7' → 7 */
    /* 0x38 */ 0x09,  /* '8' → 8 */
    /* 0x39 */ 0x0A,  /* '9' → 9 */
    /* 0x3A */ 0x27,  /* ':' → ; (shifted) */
    /* 0x3B */ 0x27,  /* ';' → ; */
    /* 0x3C */ 0x33,  /* '<' → , (shifted) */
    /* 0x3D */ 0x0D,  /* '=' → = */
    /* 0x3E */ 0x34,  /* '>' → . (shifted) */
    /* 0x3F */ 0x35,  /* '?' → / (shifted) */
    /* 0x40 */ 0x03,  /* '@' → 2 (shifted) */
    /* 0x41 */ 0x1E,  /* 'A' → A */
    /* 0x42 */ 0x30,  /* 'B' → B */
    /* 0x43 */ 0x2E,  /* 'C' → C */
    /* 0x44 */ 0x20,  /* 'D' → D */
    /* 0x45 */ 0x12,  /* 'E' → E */
    /* 0x46 */ 0x21,  /* 'F' → F */
    /* 0x47 */ 0x22,  /* 'G' → G */
    /* 0x48 */ 0x23,  /* 'H' → H */
    /* 0x49 */ 0x17,  /* 'I' → I */
    /* 0x4A */ 0x24,  /* 'J' → J */
    /* 0x4B */ 0x25,  /* 'K' → K */
    /* 0x4C */ 0x26,  /* 'L' → L */
    /* 0x4D */ 0x32,  /* 'M' → M */
    /* 0x4E */ 0x31,  /* 'N' → N */
    /* 0x4F */ 0x18,  /* 'O' → O */
    /* 0x50 */ 0x19,  /* 'P' → P */
    /* 0x51 */ 0x10,  /* 'Q' → Q */
    /* 0x52 */ 0x13,  /* 'R' → R */
    /* 0x53 */ 0x1F,  /* 'S' → S */
    /* 0x54 */ 0x14,  /* 'T' → T */
    /* 0x55 */ 0x16,  /* 'U' → U */
    /* 0x56 */ 0x2F,  /* 'V' → V */
    /* 0x57 */ 0x11,  /* 'W' → W */
    /* 0x58 */ 0x2D,  /* 'X' → X */
    /* 0x59 */ 0x15,  /* 'Y' → Y */
    /* 0x5A */ 0x2C,  /* 'Z' → Z */
    /* 0x5B */ 0x1A,  /* '[' → [ */
    /* 0x5C */ 0x2B,  /* '\' → \ */
    /* 0x5D */ 0x1B,  /* ']' → ] */
    /* 0x5E */ 0x07,  /* '^' → 6 (shifted) */
    /* 0x5F */ 0x0C,  /* '_' → - (shifted) */
    /* 0x60 */ 0x29,  /* '`' → ` */
    /* 0x61 */ 0x1E,  /* 'a' → A */
    /* 0x62 */ 0x30,  /* 'b' → B */
    /* 0x63 */ 0x2E,  /* 'c' → C */
    /* 0x64 */ 0x20,  /* 'd' → D */
    /* 0x65 */ 0x12,  /* 'e' → E */
    /* 0x66 */ 0x21,  /* 'f' → F */
    /* 0x67 */ 0x22,  /* 'g' → G */
    /* 0x68 */ 0x23,  /* 'h' → H */
    /* 0x69 */ 0x17,  /* 'i' → I */
    /* 0x6A */ 0x24,  /* 'j' → J */
    /* 0x6B */ 0x25,  /* 'k' → K */
    /* 0x6C */ 0x26,  /* 'l' → L */
    /* 0x6D */ 0x32,  /* 'm' → M */
    /* 0x6E */ 0x31,  /* 'n' → N */
    /* 0x6F */ 0x18,  /* 'o' → O */
    /* 0x70 */ 0x19,  /* 'p' → P */
    /* 0x71 */ 0x10,  /* 'q' → Q */
    /* 0x72 */ 0x13,  /* 'r' → R */
    /* 0x73 */ 0x1F,  /* 's' → S */
    /* 0x74 */ 0x14,  /* 't' → T */
    /* 0x75 */ 0x16,  /* 'u' → U */
    /* 0x76 */ 0x2F,  /* 'v' → V */
    /* 0x77 */ 0x11,  /* 'w' → W */
    /* 0x78 */ 0x2D,  /* 'x' → X */
    /* 0x79 */ 0x15,  /* 'y' → Y */
    /* 0x7A */ 0x2C,  /* 'z' → Z */
    /* 0x7B */ 0x1A,  /* '{' → [ (shifted) */
    /* 0x7C */ 0x2B,  /* '|' → \ (shifted) */
    /* 0x7D */ 0x1B,  /* '}' → ] (shifted) */
    /* 0x7E */ 0x29,  /* '~' → ` (shifted) */
    /* 0x7F */ 0x0E,  /* DEL → Backspace */
};

uint8_t ascii_to_scancode(char c) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 128) return 0;
    return s_ascii_scancode[uc];
}

/* --------------------------------------------------------------------------
   BTN_* → AT Set 1 make code table. Terminated by {0, 0}.
   BTN_MENU is intentionally omitted — it's the system menu, not a PC key.
   -------------------------------------------------------------------------- */

const btn_scancode_t btn_scancode_table[] = {
    { BTN_UP,        0x48 },  /* Arrow Up */
    { BTN_DOWN,      0x50 },  /* Arrow Down */
    { BTN_LEFT,      0x4B },  /* Arrow Left */
    { BTN_RIGHT,     0x4D },  /* Arrow Right */
    { BTN_ENTER,     0x1C },  /* Enter */
    { BTN_ESC,       0x01 },  /* Escape */
    { BTN_BACKSPACE, 0x0E },  /* Backspace */
    { BTN_TAB,       0x0F },  /* Tab */
    { BTN_DEL,       0x53 },  /* Delete */
    { BTN_SHIFT,     0x2A },  /* Left Shift */
    { BTN_CTRL,      0x1D },  /* Left Ctrl */
    { BTN_ALT,       0x38 },  /* Left Alt */
    { BTN_F1,        0x3B },  /* F1 */
    { BTN_F2,        0x3C },  /* F2 */
    { BTN_F3,        0x3D },  /* F3 */
    { BTN_F4,        0x3E },  /* F4 */
    { BTN_F5,        0x3F },  /* F5 */
    { BTN_F6,        0x40 },  /* F6 */
    { BTN_F7,        0x41 },  /* F7 */
    { BTN_F8,        0x42 },  /* F8 */
    { BTN_F9,        0x43 },  /* F9 */
    { 0, 0 }  /* sentinel */
};
