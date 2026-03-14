#pragma once

// PIO PSRAM Bulk Transfer Driver
// Supports up to 8187 bytes per transaction (300x improvement over polpo library)
// Uses 16-bit bit counters instead of 8-bit.
//
// This is a separate driver from pio_psram.h because it uses a different PIO program.
// Use this for large transfers (video frames, audio buffers).
// Use pio_psram.h for small transfers (single bytes/words).

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum bytes per bulk transfer
#define PIO_PSRAM_BULK_MAX_WRITE  8187  // (65535 - 32) / 8
#define PIO_PSRAM_BULK_MAX_READ   8191  // 65535 / 8

// Initialize the bulk transfer PIO state machine.
// Returns true on success.
bool pio_psram_bulk_init(void);

// Check if bulk driver is initialized.
bool pio_psram_bulk_available(void);

// Bulk write: write `len` bytes to PIO PSRAM address `addr`.
// Len must be <= PIO_PSRAM_BULK_MAX_WRITE.
// For larger transfers, use pio_psram_bulk_write_large().
void pio_psram_bulk_write(uint32_t addr, const uint8_t *src, uint32_t len);

// Bulk read: read `len` bytes from PIO PSRAM address `addr` into `dst`.
// Len must be <= PIO_PSRAM_BULK_MAX_READ.
// For larger transfers, use pio_psram_bulk_read_large().
void pio_psram_bulk_read(uint32_t addr, uint8_t *dst, uint32_t len);

// Large write: write arbitrary length, chunked internally.
void pio_psram_bulk_write_large(uint32_t addr, const uint8_t *src, uint32_t len);

// Large read: read arbitrary length, chunked internally.
void pio_psram_bulk_read_large(uint32_t addr, uint8_t *dst, uint32_t len);

// Get transfer statistics (for debugging/performance tuning).
typedef struct {
    uint32_t write_calls;
    uint32_t read_calls;
    uint32_t write_bytes;
    uint32_t read_bytes;
    uint32_t write_chunks;   // number of DMA chunks
    uint32_t read_chunks;
} pio_psram_bulk_stats_t;

void pio_psram_bulk_get_stats(pio_psram_bulk_stats_t *stats);
void pio_psram_bulk_reset_stats(void);

#ifdef __cplusplus
}
#endif
