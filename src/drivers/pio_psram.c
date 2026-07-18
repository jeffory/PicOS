#include "pio_psram.h"
#include "pio_psram_bulk.h"
#include "pio_psram_qpi.h"
#include "../hardware.h"

#include "pico/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PSRAM_MODE_NONE = 0,
    PSRAM_MODE_QPI,
    PSRAM_MODE_SERIAL,
} psram_mode_t;

static psram_mode_t s_mode = PSRAM_MODE_NONE;

bool pio_psram_init(void) {
    if (s_mode != PSRAM_MODE_NONE) return true;

    if (pio_psram_qpi_init()) {
        s_mode = PSRAM_MODE_QPI;
        printf("[PIO_PSRAM] QPI mode @ %lu kHz SPI\n",
               (unsigned long)pio_psram_qpi_spi_khz());
        return true;
    }

    if (pio_psram_bulk_init()) {
        s_mode = PSRAM_MODE_SERIAL;
        printf("[PIO_PSRAM] Serial fallback mode (25 MHz SPI)\n");
        return true;
    }

    printf("[PIO_PSRAM] Not available\n");
    return false;
}

void pio_psram_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    if (s_mode == PSRAM_MODE_QPI)
        pio_psram_qpi_read(addr, dst, len);
    else if (s_mode == PSRAM_MODE_SERIAL)
        pio_psram_bulk_read(addr, dst, len);
}

void pio_psram_write(uint32_t addr, const uint8_t *src, uint32_t len) {
    if (s_mode == PSRAM_MODE_QPI)
        pio_psram_qpi_write(addr, src, len);
    else if (s_mode == PSRAM_MODE_SERIAL)
        pio_psram_bulk_write(addr, src, len);
}

uint32_t pio_psram_size(void) {
    return (s_mode != PSRAM_MODE_NONE) ? PIO_PSRAM_SIZE : 0;
}

bool pio_psram_available(void) {
    return s_mode != PSRAM_MODE_NONE;
}

void pio_psram_set_sysclk(uint32_t sys_khz) {
    if (s_mode == PSRAM_MODE_QPI)
        pio_psram_qpi_set_sysclk(sys_khz);
    else if (s_mode == PSRAM_MODE_SERIAL)
        pio_psram_bulk_set_sysclk(sys_khz);
}

const char *pio_psram_mode_str(void) {
    switch (s_mode) {
    case PSRAM_MODE_QPI:    return "qpi";
    case PSRAM_MODE_SERIAL: return "serial";
    default:                return "none";
    }
}

// ── Dev-command test ─────────────────────────────────────────────────────────

#define DBG_BLOCK 4096u

static void debug_fill_pattern(uint8_t *buf, uint32_t addr) {
    for (uint32_t i = 0; i < DBG_BLOCK; i++)
        buf[i] = (uint8_t)((addr >> 12) * 197u + i * 13u + 5u);
}

void pio_psram_debug_test(bool full) {
    printf("[PSRAM] mode=%s\n", pio_psram_mode_str());
    if (s_mode == PSRAM_MODE_NONE) return;
    if (s_mode == PSRAM_MODE_QPI)
        printf("[PSRAM] spi_clock=%lu kHz\n",
               (unsigned long)pio_psram_qpi_spi_khz());
    pio_psram_qpi_print_diag();

    uint8_t *buf = malloc(DBG_BLOCK);   // SRAM heap; freed below
    if (!buf) {
        printf("[PSRAM] SRAM alloc failed\n");
        return;
    }

    // Throughput: 256KB write then read, app region (above mp3/video pools).
    const uint32_t bench_bytes = 256u * 1024u;
    debug_fill_pattern(buf, PIO_PSRAM_APP_BASE);
    uint64_t t0 = time_us_64();
    for (uint32_t off = 0; off < bench_bytes; off += DBG_BLOCK)
        pio_psram_write(PIO_PSRAM_APP_BASE + off, buf, DBG_BLOCK);
    uint64_t t1 = time_us_64();
    for (uint32_t off = 0; off < bench_bytes; off += DBG_BLOCK)
        pio_psram_read(PIO_PSRAM_APP_BASE + off, buf, DBG_BLOCK);
    uint64_t t2 = time_us_64();
    printf("[PSRAM] write 256KB: %lu us (%lu KB/s)\n",
           (unsigned long)(t1 - t0),
           (unsigned long)((uint64_t)bench_bytes * 1000000u / (t1 - t0) / 1024u));
    printf("[PSRAM] read  256KB: %lu us (%lu KB/s)\n",
           (unsigned long)(t2 - t1),
           (unsigned long)((uint64_t)bench_bytes * 1000000u / (t2 - t1) / 1024u));

    // Integrity: two-pass sweep (write everything, then verify everything).
    // Two passes catch address aliasing that immediate readback would miss.
    uint32_t start = full ? 0u : PIO_PSRAM_APP_BASE;
    uint32_t end = full ? PIO_PSRAM_SIZE
                        : (PIO_PSRAM_APP_BASE + 1024u * 1024u);
    printf("[PSRAM] integrity sweep %06lX-%06lX (destructive)...\n",
           (unsigned long)start, (unsigned long)(end - 1));

    for (uint32_t addr = start; addr < end; addr += DBG_BLOCK) {
        debug_fill_pattern(buf, addr);
        pio_psram_write(addr, buf, DBG_BLOCK);
    }
    uint32_t errors = 0;
    uint32_t first_bad = 0;
    uint8_t *expect = malloc(DBG_BLOCK);
    if (!expect) {
        printf("[PSRAM] SRAM alloc failed (verify)\n");
        free(buf);
        return;
    }
    for (uint32_t addr = start; addr < end; addr += DBG_BLOCK) {
        debug_fill_pattern(expect, addr);
        memset(buf, 0, DBG_BLOCK);
        pio_psram_read(addr, buf, DBG_BLOCK);
        for (uint32_t i = 0; i < DBG_BLOCK; i++) {
            if (buf[i] != expect[i]) {
                if (errors == 0) first_bad = addr + i;
                if (errors < 6)
                    printf("[PSRAM]   mismatch @%06lX exp %02X got %02X\n",
                           (unsigned long)(addr + i), expect[i], buf[i]);
                errors++;
            }
        }
    }
    if (errors)
        printf("[PSRAM] integrity FAIL: %lu bad bytes, first at %06lX\n",
               (unsigned long)errors, (unsigned long)first_bad);
    else
        printf("[PSRAM] integrity OK\n");

    free(expect);
    free(buf);
}
