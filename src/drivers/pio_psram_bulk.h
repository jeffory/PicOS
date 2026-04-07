#pragma once

// PIO PSRAM Streaming Bulk Transfer Driver
// Uses 8-bit chunk counters with CS held low across chunks (PSRAM sequential mode).
// ~3% overhead vs ideal for large transfers.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Max data bytes per chunk (8-bit counter: 255 bits max)
#define PIO_PSRAM_BULK_CHUNK_WRITE  27    // (255 - 32) / 8  [32 bits = cmd + addr overhead]
#define PIO_PSRAM_BULK_CHUNK_READ   31    // 255 / 8

// Initialize the bulk transfer PIO state machine.
// Returns true on success.
bool pio_psram_bulk_init(void);

// Check if bulk driver is initialized.
bool pio_psram_bulk_available(void);

// Write `len` bytes to PIO PSRAM address `addr`. Any size supported.
void pio_psram_bulk_write(uint32_t addr, const uint8_t *src, uint32_t len);

// Read `len` bytes from PIO PSRAM address `addr` into `dst`. Any size supported.
void pio_psram_bulk_read(uint32_t addr, uint8_t *dst, uint32_t len);

// Get transfer statistics (for debugging/performance tuning).
typedef struct {
    uint32_t write_calls;
    uint32_t read_calls;
    uint32_t write_bytes;
    uint32_t read_bytes;
} pio_psram_bulk_stats_t;

void pio_psram_bulk_get_stats(pio_psram_bulk_stats_t *stats);
void pio_psram_bulk_reset_stats(void);

#ifdef __cplusplus
}
#endif
