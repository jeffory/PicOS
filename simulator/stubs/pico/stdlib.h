// pico/stdlib.h stub
#ifndef PICO_STDLIB_H
#define PICO_STDLIB_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "time.h"

// Standard initialization
static inline void stdio_init_all(void) {}
static inline void setup_default_uart(void) {}
static inline void stdio_flush(void) { fflush(stdout); }

// Tight loop hint (no-op on simulator)
static inline void tight_loop_contents(void) {}

// Wait functions are defined in time.h

#endif // PICO_STDLIB_H
