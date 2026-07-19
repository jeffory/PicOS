#pragma once
#include <stdint.h>

/* Convert ASCII character to AT Set 1 make code. Returns 0 if unmapped. */
uint8_t ascii_to_scancode(char c);

/* Map of PicOS BTN_* bits to AT Set 1 make codes (0 = end of table) */
typedef struct {
    uint32_t btn_mask;
    uint8_t  scancode;
} btn_scancode_t;

extern const btn_scancode_t btn_scancode_table[];
