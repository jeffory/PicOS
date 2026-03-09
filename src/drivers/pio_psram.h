#pragma once

// PIO PSRAM driver — thin wrapper around the polpo rp2040-psram library.
// Drives the 8MB PSRAM chip on the PicoCalc v2.0 mainboard via PIO1 SPI.
// This bus is completely independent of the Pimoroni QMI PSRAM (Lua heap)
// and the Flash XIP cache, so reads/writes cause zero cache pressure.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise PIO1 state machine, DMA channels, and reset the PSRAM chip.
// Returns true on success.  Non-fatal if chip is not present.
bool pio_psram_init(void);

// Read `len` bytes from PIO PSRAM address `addr` into `dst`.
void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len);

// Write `len` bytes from `src` to PIO PSRAM address `addr`.
void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len);

// Returns total size in bytes (8MB) if initialised, 0 otherwise.
uint32_t pio_psram_size(void);

// Returns true after successful init.
bool pio_psram_available(void);

#ifdef __cplusplus
}
#endif
