#pragma once

#include <stdint.h>

#define FONT_6X11_WIDTH 6
#define FONT_6X11_HEIGHT 11
#define FONT_6X11_START 0x20
#define FONT_6X11_END 0x7E
#define FONT_6X11_COUNT (FONT_6X11_END - FONT_6X11_START + 1)

extern const uint16_t font_6x11[FONT_6X11_COUNT][FONT_6X11_WIDTH];

int font_6x11_width(char c);

const uint16_t* font_6x11_glyph(char c);
