// Stub for Arduino pgmspace.h — not needed on RP2350 (unified address space)
#ifndef PGMSPACE_H
#define PGMSPACE_H

#include <string.h>

#define PSTR(s)     (s)
#define memcpy_P    memcpy
#define pgm_read_byte(addr)   (*(const unsigned char *)(addr))
#define pgm_read_word(addr)   (*(const unsigned short *)(addr))
#define pgm_read_dword(addr)  (*(const unsigned long *)(addr))

#endif
