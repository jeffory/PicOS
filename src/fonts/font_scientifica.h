#pragma once

#include <stdint.h>

#define FONT_SCI_WIDTH  6
#define FONT_SCI_HEIGHT 12
#define FONT_SCI_FIRST  0x20
#define FONT_SCI_LAST   0x9F
#define FONT_SCI_COUNT  128

// Row-major, MSB = leftmost pixel, 12 bytes per glyph, 0x20..0x9F.
// 0x7F is blank; 0x80..0x9F are box-drawing glyphs shared by both faces.
extern const uint8_t font_scientifica[FONT_SCI_COUNT][12];
extern const uint8_t font_scientifica_bold[FONT_SCI_COUNT][12];

const uint8_t* font_scientifica_glyph(char c);
const uint8_t* font_scientifica_bold_glyph(char c);
