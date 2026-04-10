/*
  DOS86 — VRAM-to-RGB565 renderer.
  Converts emulated PC video memory to a 320x200 RGB565 framebuffer.
*/

#pragma once
#include <stdint.h>
#include <stdbool.h>

bool render_frame(uint16_t *out_buf);
int render_get_width(void);
int render_get_height(void);
