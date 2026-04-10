/*
  DOS86 — BIOS interrupt handlers: INT 16h (keyboard), INT 1Ah (time).
*/

#pragma once
#include <stdint.h>

void bios_int16h(void);   /* BIOS keyboard services */
void bios_int1ah(void);   /* BIOS time services */
