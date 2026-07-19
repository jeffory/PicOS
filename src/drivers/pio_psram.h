#pragma once

// PIO PSRAM driver — unified API wrapping the streaming bulk transfer driver.
// Drives the 8MB PSRAM chip on the PicoCalc v2.0 mainboard via PIO1 SPI.
// This bus is completely independent of the Pimoroni QMI PSRAM (Lua heap)
// and the Flash XIP cache, so reads/writes cause zero cache pressure.
//
// Uses streaming protocol: CS stays asserted across 8-bit chunk boundaries
// (PSRAM sequential mode), giving ~3% overhead for large transfers.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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

// Rescale the PIO clock divider after a sysclk change (called from
// launcher_apply_clock). Keeps the SPI clock at or below the active tier's
// validated rate. No-op when PSRAM is unavailable.
void pio_psram_set_sysclk(uint32_t sys_khz);

// Active mode: "qpi", "serial", or "none".
const char *pio_psram_mode_str(void);

// Dev-command test: prints mode, throughput (256KB write+read), and a
// two-pass pattern integrity sweep. full=false sweeps 1MB of the app region;
// full=true sweeps the whole 8MB. DESTRUCTIVE to PSRAM contents — only run
// from the launcher with no audio/video active.
void pio_psram_debug_test(bool full);

// Dev-command stress: hammers the page-end read-disturb path (adversarial
// patterns at page ends across the chip, `iters` reads each) plus sequential
// readback of the MP3 ring region. Reports residual error counts — anything
// nonzero at the operating point is audible as audio crackle. DESTRUCTIVE to
// the MP3 ring + app region — run from the launcher only.
void pio_psram_stress_test(uint32_t iters);

#ifdef __cplusplus
}
#endif
