#include "pio_psram.h"
#include "../hardware.h"

// polpo library — pin defines must be set before including the header.
// We set them via CMake target_compile_definitions (PSRAM_PIN_CS etc.).
#include "psram_spi.h"

#include "hardware/xip_cache.h"
#include <stdio.h>

// Max bulk transfer per polpo call — the read_command[1] field is a uint8_t
// holding (count * 8) bits, so max 31 bytes per read call.  For writes,
// write_command[0] = (4 + count) * 8, so max 27 bytes per write call.
// We chunk larger transfers to stay within these limits.
#define PIO_PSRAM_MAX_READ_CHUNK  31
#define PIO_PSRAM_MAX_WRITE_CHUNK 27

static psram_spi_inst_t s_psram_spi;
static bool s_available = false;

// --- XIP cache coherency helpers ---
// The polpo library uses DMA to transfer data to/from PIO.  If the caller's
// buffer lives in cached QMI PSRAM (0x11xxxxxx), DMA may see stale data
// (write-back dirty lines not yet flushed) or the CPU may read stale cached
// values after DMA writes.  We detect this case and redirect DMA through
// the uncached alias (0x15xxxxxx), with appropriate cache maintenance.

static inline bool is_cached_psram(const void *ptr) {
    return ((uintptr_t)ptr >> 24) == 0x11;
}

// Offset from cached (0x11) to uncached (0x15) QMI PSRAM alias
#define XIP_UNCACHED_OFFSET 0x04000000

bool pio_psram_init(void) {
    if (s_available) return true;

    printf("[PIO_PSRAM] Initialising on PIO1 (CS=GP%d SCK=GP%d MOSI=GP%d MISO=GP%d)...\n",
           PIO_PSRAM_PIN_CS, PIO_PSRAM_PIN_SCK, PIO_PSRAM_PIN_MOSI, PIO_PSRAM_PIN_MISO);

    // clkdiv=1.0 at 200MHz sys_clk → 100MHz PIO clock → ~50MHz SPI
    // fudge=true for reliable reads (extra clock cycle before read phase)
    s_psram_spi = psram_spi_init_clkdiv(PIO_PSRAM_PIO, -1, 1.0f, true);

    // Quick self-test: write a pattern and read it back
    uint8_t test_pattern[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t readback[4] = {0};

    for (int i = 0; i < 4; i++)
        psram_write8(&s_psram_spi, (uint32_t)i, test_pattern[i]);

    for (int i = 0; i < 4; i++)
        readback[i] = psram_read8(&s_psram_spi, (uint32_t)i);

    if (readback[0] != 0xDE || readback[1] != 0xAD ||
        readback[2] != 0xBE || readback[3] != 0xEF) {
        printf("[PIO_PSRAM] Self-test FAILED (read %02X%02X%02X%02X, expected DEADBEEF)\n",
               readback[0], readback[1], readback[2], readback[3]);
        psram_spi_uninit(s_psram_spi, true);
        return false;
    }

    s_available = true;
    printf("[PIO_PSRAM] Initialised: %luMB\n",
           (unsigned long)(PIO_PSRAM_SIZE / (1024 * 1024)));
    return true;
}

void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    if (is_cached_psram(dst)) {
        // Flush dirty cache lines so stale evictions don't overwrite DMA data,
        // then redirect DMA to the uncached alias.
        __asm volatile ("dsb sy");
        xip_cache_clean_all();          // also invalidates (RP2350 E11)
        __asm volatile ("dsb sy" ::: "memory");
        dst = (uint8_t *)((uintptr_t)dst + XIP_UNCACHED_OFFSET);
    }
    while (len > 0) {
        uint32_t chunk = (len > PIO_PSRAM_MAX_READ_CHUNK) ? PIO_PSRAM_MAX_READ_CHUNK : len;
        psram_read(&s_psram_spi, addr, dst, chunk);
        addr += chunk;
        dst  += chunk;
        len  -= chunk;
    }
}

void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len) {
    if (is_cached_psram(src)) {
        // Flush write-back dirty lines to QMI PSRAM so DMA reads correct data,
        // then redirect DMA to the uncached alias.
        __asm volatile ("dsb sy");
        xip_cache_clean_all();          // also invalidates (RP2350 E11)
        __asm volatile ("dsb sy" ::: "memory");
        src = (const uint8_t *)((uintptr_t)src + XIP_UNCACHED_OFFSET);
    }
    while (len > 0) {
        uint32_t chunk = (len > PIO_PSRAM_MAX_WRITE_CHUNK) ? PIO_PSRAM_MAX_WRITE_CHUNK : len;
        psram_write(&s_psram_spi, addr, src, chunk);
        addr += chunk;
        src  += chunk;
        len  -= chunk;
    }
}

uint32_t pio_psram_size(void) {
    return s_available ? PIO_PSRAM_SIZE : 0;
}

bool pio_psram_available(void) {
    return s_available;
}
