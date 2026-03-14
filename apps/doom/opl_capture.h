// OPL register capture for offline debugging
// Writes a binary .opcl file of all OPL register writes + sample advances
// Enable with -DOPL_CAPTURE in Makefile

#ifndef OPL_CAPTURE_H
#define OPL_CAPTURE_H

#include <stdint.h>

#ifdef OPL_CAPTURE

void opl_capture_init(void);
void opl_capture_reg_write(uint8_t reg, uint8_t val);
void opl_capture_advance(int sample_count);
void opl_capture_stop(void);

#else

#define opl_capture_init()
#define opl_capture_reg_write(r, v)
#define opl_capture_advance(n)
#define opl_capture_stop()

#endif

#endif // OPL_CAPTURE_H
