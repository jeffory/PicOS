#pragma once

// PIO PSRAM driver — unified API wrapping the bulk transfer driver.
// Drives the 8MB PSRAM chip on the PicoCalc v2.0 mainboard via PIO1 SPI.
// This bus is completely independent of the Pimoroni QMI PSRAM (Lua heap)
// and the Flash XIP cache, so reads/writes cause zero cache pressure.
//
// The bulk driver supports up to 8KB per transaction (300x faster than
// the original 27-byte chunk driver for large transfers).

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum bytes per bulk transfer
#define PIO_PSRAM_MAX_WRITE  8187
#define PIO_PSRAM_MAX_READ   8191

// Memory layout constants for PIO PSRAM
#define PIO_PSRAM_MP3_RING_BASE    0x0000
#define PIO_PSRAM_MP3_RING_SIZE    (32 * 1024)
#define PIO_PSRAM_VIDEO_BASE       (32 * 1024)
#define PIO_PSRAM_VIDEO_SIZE       (256 * 1024)
#define PIO_PSRAM_APP_BASE         (288 * 1024)

// Initialise PIO1 state machine, DMA channels, and reset the PSRAM chip.
// Returns true on success.  Non-fatal if chip is not present.
bool pio_psram_init(void);

// Read `len` bytes from PIO PSRAM address `addr` into `dst`.
// Uses bulk transfers for efficiency.
void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len);

// Write `len` bytes from `src` to PIO PSRAM address `addr`.
// Uses bulk transfers for efficiency.
void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len);

// Returns total size in bytes (8MB) if initialised, 0 otherwise.
uint32_t pio_psram_size(void);

// Returns true after successful init.
bool pio_psram_available(void);

#ifdef __cplusplus
}
#endif
