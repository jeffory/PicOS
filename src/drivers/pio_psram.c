#include "pio_psram.h"
#include "pio_psram_bulk.h"
#include "../hardware.h"

#include <stdio.h>

static bool s_available = false;

bool pio_psram_init(void) {
    if (s_available) return true;

    // Use bulk driver exclusively (8KB chunks, 300x faster for large transfers)
    if (pio_psram_bulk_init()) {
        s_available = true;
        printf("[PIO_PSRAM] Initialised bulk driver (8KB max per transfer)\n");
        return true;
    }

    printf("[PIO_PSRAM] Bulk driver init failed - PIO PSRAM not available\n");
    return false;
}

void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    if (!s_available) return;
    pio_psram_bulk_read_large(addr, dst, len);
}

void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len) {
    if (!s_available) return;
    pio_psram_bulk_write_large(addr, src, len);
}

uint32_t pio_psram_size(void) {
    return s_available ? PIO_PSRAM_SIZE : 0;
}

bool pio_psram_available(void) {
    return s_available;
}
