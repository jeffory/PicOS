#pragma once

#include <stdint.h>

#define FONT_SCI_WIDTH  6
#define FONT_SCI_HEIGHT 12
#define FONT_SCI_COUNT  95

#define FONT_SCI_EXTENDED_START 0x80
#define FONT_SCI_EXTENDED_COUNT 32

extern const uint8_t font_scientifica[95][12];
extern const uint8_t font_scientifica_bold[95][12];
extern const uint8_t font_scientifica_extended[32][12];

const uint8_t* font_scientifica_glyph(char c);
const uint8_t* font_scientifica_bold_glyph(char c);
