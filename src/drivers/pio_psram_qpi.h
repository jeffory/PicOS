#pragma once

// PIO PSRAM QPI (quad) driver — 4-bit transfers on SIO0-3 (GP2-GP5).
// Probed at boot by pio_psram.c; falls back to the serial bulk driver if the
// self-test fails (SIO2/3 not wired, timing margin, chip absent).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Probe and initialise QPI mode. Tries a 50 MHz SPI clock, then 25 MHz, each
// gated by a write/readback self-test. Returns false with all PIO/DMA
// resources released so the serial fallback can claim them.
bool pio_psram_qpi_init(void);

bool pio_psram_qpi_available(void);

// Chunked, tCEM-compliant, DMA-fed transfers. Any addr/len.
void pio_psram_qpi_read(uint32_t addr, uint8_t *dst, uint32_t len);
void pio_psram_qpi_write(uint32_t addr, const uint8_t *src, uint32_t len);

// Rescale the PIO clock divider after a sysclk change so the SPI clock stays
// at or below the tier target chosen at init. Safe from any core.
void pio_psram_qpi_set_sysclk(uint32_t sys_khz);

// Currently configured SPI clock in kHz (0 if not available).
uint32_t pio_psram_qpi_spi_khz(void);

#ifdef __cplusplus
}
#endif
