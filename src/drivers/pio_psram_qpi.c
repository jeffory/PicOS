#include "pio_psram_qpi.h"
#include "../hardware.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/xip_cache.h"
#include "pico/mutex.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include <stdio.h>
#include <string.h>

#include "pio_psram_qpi.pio.h"

// XIP cache coherency — same scheme as pio_psram_bulk.c: buffers in QMI PSRAM
// may be addressed through the cached 0x11 alias; DMA must see flushed data
// through the uncached 0x15 alias.
static inline bool is_cached_psram(const void *ptr) {
    return ((uintptr_t)ptr >> 24) == 0x11;
}
#define XIP_UNCACHED_OFFSET 0x04000000

static inline const uint8_t *flush_and_uncache(const void *ptr) {
    __asm volatile ("dsb sy");
    xip_cache_clean_all();
    __asm volatile ("dsb sy" ::: "memory");
    return (const uint8_t *)((uintptr_t)ptr + XIP_UNCACHED_OFFSET);
}

static inline uint8_t *flush_and_uncache_dst(void *ptr) {
    __asm volatile ("dsb sy");
    xip_cache_clean_all();
    __asm volatile ("dsb sy" ::: "memory");
    return (uint8_t *)((uintptr_t)ptr + XIP_UNCACHED_OFFSET);
}

// PSRAM commands (ESP-PSRAM64H / APS6404L / LY68L6400 family)
#define PSRAM_CMD_RESET_EN   0x66
#define PSRAM_CMD_RESET      0x99
#define PSRAM_CMD_ENTER_QPI  0x35
#define PSRAM_CMD_EXIT_QPI   0xF5
#define PSRAM_CMD_QPI_READ   0xEB  // 6 dummy clocks, max 84 MHz
#define PSRAM_CMD_QPI_WRITE  0x38

// 8-bit nibble counters cap transactions; tCEM caps them further (see
// qpi_apply_timing). Absolute maxima from the counters:
#define QPI_CHUNK_WRITE_MAX  120   // (4+120)*2 = 248 <= 255 write nibbles
#define QPI_CHUNK_READ_MAX   124   // 124*2 = 248 <= 255 read nibbles

// State
static PIO s_pio = NULL;
static int s_sm = -1;
static uint s_prog_offs = 0;
static bool s_available = false;
static mutex_t s_mutex;

static uint32_t s_target_khz = 0;   // tier target (50000 or 25000)
static uint32_t s_spi_khz = 0;      // actual SPI clock after divider
static uint32_t s_chunk_write = 0;  // data bytes per write transaction
static uint32_t s_chunk_read = 0;   // data bytes per read transaction

static int s_write_dma_chan = -1;
static int s_read_dma_chan = -1;
static dma_channel_config s_write_dma_cfg;
static dma_channel_config s_read_dma_cfg;

// Boot-probe diagnostics, retrievable after teardown via print_diag.
typedef struct {
    uint32_t tier_khz;
    uint32_t addr;
    uint8_t wrote[8];
    uint8_t got[8];
    bool valid;
} qpi_diag_fail_t;
static qpi_diag_fail_t s_diag_fail[2];
static uint8_t s_diag_readid_raw[8];
static int s_diag_readid_count = 0;

// Narrow AHB write replicates the byte across the 32-bit word; with autopull 8
// and left shift the top byte is consumed — same trick as pio_psram_bulk.c.
static inline void qpi_put_byte(uint8_t val) {
    while (pio_sm_is_tx_fifo_full(s_pio, s_sm))
        tight_loop_contents();
    *(volatile uint8_t *)&s_pio->txf[s_sm] = val;
}

static void qpi_wait_tx_idle(void) {
    while (!pio_sm_is_tx_fifo_empty(s_pio, s_sm))
        tight_loop_contents();
}

// Send a command in native QPI format (both nibbles on SIO0-3).
static void qpi_send_quad_cmd(uint8_t cmd) {
    qpi_put_byte(2);     // 2 write nibbles
    qpi_put_byte(0);     // no read
    qpi_put_byte(cmd);
    qpi_wait_tx_idle();
}

// Send a command in 1-bit serial format through the 4-bit program: each SPI
// bit becomes one nibble whose bit0 drives SIO0. The chip's SO (SIO1) is
// Hi-Z during command input, so driving SIO1-3 low causes no contention.
static void qpi_send_serial_cmd(uint8_t cmd) {
    qpi_put_byte(8);     // 8 nibble clocks = 8 serial bits
    qpi_put_byte(0);     // no read
    for (int i = 0; i < 4; i++) {
        uint8_t hi = (cmd >> (7 - 2 * i)) & 1u;
        uint8_t lo = (cmd >> (6 - 2 * i)) & 1u;
        qpi_put_byte((uint8_t)((hi << 4) | lo));
    }
    qpi_wait_tx_idle();
}

// Compute divider + tCEM-compliant chunk sizes for the current sysclk and
// tier target. SPI clock = sysclk / (2 * div); integer divider avoids
// fractional-divider jitter on the SPI edges.
static void qpi_apply_timing(uint32_t sys_khz) {
    uint32_t div = (sys_khz + 2 * s_target_khz - 1) / (2 * s_target_khz);
    if (div < 1) div = 1;
    s_spi_khz = sys_khz / (2 * div);

    // Keep CS low <= ~6us (chip tCEM max is 8us; 2us margin).
    uint32_t max_clocks = s_spi_khz * 6 / 1000;
    uint32_t wr = (max_clocks > 10) ? (max_clocks - 8) / 2 : 1;
    uint32_t rd = (max_clocks > 18) ? (max_clocks - 16) / 2 : 1;
    if (wr > QPI_CHUNK_WRITE_MAX) wr = QPI_CHUNK_WRITE_MAX;
    if (rd > QPI_CHUNK_READ_MAX) rd = QPI_CHUNK_READ_MAX;
    s_chunk_write = wr;
    s_chunk_read = rd;

    pio_sm_set_clkdiv_int_frac(s_pio, s_sm, (uint16_t)div, 0);
    pio_sm_clkdiv_restart(s_pio, s_sm);
}

// Wake sequence: safe from any prior chip state.
// A quad-format 0xF5 exits a chip left in QPI by a soft reset; if the chip is
// already in SPI mode the 2-clock partial command is discarded at CS rise.
// Then a normal serial reset and QPI entry.
static void qpi_wake_chip(void) {
    qpi_send_quad_cmd(PSRAM_CMD_EXIT_QPI);
    busy_wait_us(100);
    qpi_send_serial_cmd(PSRAM_CMD_RESET_EN);
    busy_wait_us(50);
    qpi_send_serial_cmd(PSRAM_CMD_RESET);
    busy_wait_us(100);
    qpi_send_serial_cmd(PSRAM_CMD_ENTER_QPI);
    busy_wait_us(100);
}

// Diagnostic: serial-format Read ID (0x9F + 3 address bytes) sent through the
// QPI program with bit-in-nibble encoding. In SPI mode the chip drives SO
// (= SIO1) during the read phase, so each captured nibble carries one serial
// bit in bit1. Expected decode for this chip family: MF=0x0D, KGD=0x5D.
// Runs only on the probe-failure path, before teardown.
static void qpi_diag_read_id(void) {
    while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm))
        (void)pio_sm_get(s_pio, s_sm);

    qpi_put_byte(32);   // write nibbles: 8 cmd bits + 24 addr bits, 1 bit/clock
    qpi_put_byte(16);   // read nibbles: 16 serial bits (MF ID + KGD)
    uint8_t cmd = 0x9F;
    for (int i = 0; i < 4; i++) {
        uint8_t hi = (cmd >> (7 - 2 * i)) & 1u;
        uint8_t lo = (cmd >> (6 - 2 * i)) & 1u;
        qpi_put_byte((uint8_t)((hi << 4) | lo));
    }
    for (int i = 0; i < 12; i++)
        qpi_put_byte(0x00);   // 24 address bits, all zero

    absolute_time_t deadline = make_timeout_time_ms(50);
    int n = 0;
    while (n < 8 && !time_reached(deadline)) {
        if (!pio_sm_is_rx_fifo_empty(s_pio, s_sm))
            s_diag_readid_raw[n++] = (uint8_t)pio_sm_get(s_pio, s_sm);
    }
    s_diag_readid_count = n;
}

// Core transfer implementations. Callers hold s_mutex.
static void qpi_write_locked(uint32_t addr, const uint8_t *src, uint32_t len) {
    uint32_t remaining = len;
    const uint8_t *p = src;
    while (remaining > 0) {
        uint32_t chunk = (remaining > s_chunk_write) ? s_chunk_write : remaining;

        qpi_put_byte((uint8_t)((4 + chunk) * 2));  // write nibbles
        qpi_put_byte(0);                           // no read
        qpi_put_byte(PSRAM_CMD_QPI_WRITE);
        qpi_put_byte((addr >> 16) & 0xFF);
        qpi_put_byte((addr >> 8) & 0xFF);
        qpi_put_byte(addr & 0xFF);

        dma_channel_configure(s_write_dma_chan, &s_write_dma_cfg,
                              &s_pio->txf[s_sm], p, chunk, true);
        dma_channel_wait_for_finish_blocking(s_write_dma_chan);

        addr += chunk;
        p += chunk;
        remaining -= chunk;
    }
    qpi_wait_tx_idle();
}

static void qpi_read_locked(uint32_t addr, uint8_t *dst, uint32_t len) {
    uint32_t remaining = len;
    uint8_t *p = dst;
    while (remaining > 0) {
        uint32_t chunk = (remaining > s_chunk_read) ? s_chunk_read : remaining;

        while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm))
            (void)pio_sm_get(s_pio, s_sm);

        qpi_put_byte(14);                          // cmd 2 + addr 6 + dummy 6
        qpi_put_byte((uint8_t)(chunk * 2));        // read nibbles
        qpi_put_byte(PSRAM_CMD_QPI_READ);
        qpi_put_byte((addr >> 16) & 0xFF);
        qpi_put_byte((addr >> 8) & 0xFF);
        qpi_put_byte(addr & 0xFF);
        qpi_put_byte(0x00);                        // 6 dummy clocks driven low
        qpi_put_byte(0x00);
        qpi_put_byte(0x00);

        dma_channel_configure(s_read_dma_chan, &s_read_dma_cfg,
                              p, &s_pio->rxf[s_sm], chunk, true);
        dma_channel_wait_for_finish_blocking(s_read_dma_chan);

        addr += chunk;
        p += chunk;
        remaining -= chunk;
    }
}

// Write/readback self-test at addr 0 and 4MB. Reads twice to catch marginal
// timing. Note: 'in pins' never stalls, so an unwired bus returns garbage
// (clean failure), never a hang.
static bool qpi_self_test(void) {
    static const uint32_t addrs[] = { 0x000000u, 0x400000u };
    uint8_t pat[16], rd[16];

    for (unsigned a = 0; a < 2; a++) {
        uint32_t addr = addrs[a];
        pat[0] = 0x00; pat[1] = 0xFF; pat[2] = 0xAA; pat[3] = 0x55;
        for (int i = 4; i < 16; i++)
            pat[i] = (uint8_t)((addr >> 16) + i * 31u + 7u);

        qpi_write_locked(addr, pat, sizeof(pat));

        for (int pass = 0; pass < 2; pass++) {
            memset(rd, 0, sizeof(rd));
            qpi_read_locked(addr, rd, sizeof(rd));
            if (memcmp(pat, rd, sizeof(rd)) != 0) {
                printf("[PIO_PSRAM_QPI] self-test fail @%06lX pass %d: "
                       "wrote %02X%02X%02X%02X got %02X%02X%02X%02X\n",
                       (unsigned long)addr, pass,
                       pat[0], pat[1], pat[2], pat[3],
                       rd[0], rd[1], rd[2], rd[3]);
                unsigned slot = s_diag_fail[0].valid ? 1 : 0;
                s_diag_fail[slot].tier_khz = s_target_khz;
                s_diag_fail[slot].addr = addr;
                memcpy(s_diag_fail[slot].wrote, pat, 8);
                memcpy(s_diag_fail[slot].got, rd, 8);
                s_diag_fail[slot].valid = true;
                return false;
            }
        }
    }
    return true;
}

bool pio_psram_qpi_init(void) {
    if (s_available) return true;

    s_pio = PIO_PSRAM_PIO;
    if (!pio_can_add_program(s_pio, &psram_qpi_program)) {
        printf("[PIO_PSRAM_QPI] No PIO instruction space\n");
        return false;
    }
    s_sm = pio_claim_unused_sm(s_pio, false);
    if (s_sm < 0) {
        printf("[PIO_PSRAM_QPI] No free state machine\n");
        return false;
    }
    s_prog_offs = pio_add_program(s_pio, &psram_qpi_program);

    // clkdiv here is provisional; qpi_apply_timing() sets the real divider.
    psram_qpi_cs_init(s_pio, s_sm, s_prog_offs, 4.0f,
                      PIO_PSRAM_PIN_CS, PIO_PSRAM_PIN_SIO0);

    // Electrical profile: match the proven serial driver (4mA, fast slew on
    // CS/SCK) and keep input hysteresis ENABLED on the data pins. The PR #15
    // profile (8mA, fast slew, hysteresis off) targets 75-115 MHz; on this
    // board at 25-50 MHz it produced sparse transient bit errors (crosstalk /
    // simultaneous-switching noise on the four data lines). Data pins use
    // slow slew to cut aggressor noise; edge rate is no constraint at these
    // clocks.
    gpio_set_drive_strength(PIO_PSRAM_PIN_CS, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PIO_PSRAM_PIN_SCK, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_slew_rate(PIO_PSRAM_PIN_CS, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIO_PSRAM_PIN_SCK, GPIO_SLEW_RATE_FAST);
    static const uint data_pins[] = { PIO_PSRAM_PIN_SIO0, PIO_PSRAM_PIN_SIO1,
                                      PIO_PSRAM_PIN_SIO2, PIO_PSRAM_PIN_SIO3 };
    for (unsigned i = 0; i < 4; i++) {
        gpio_set_drive_strength(data_pins[i], GPIO_DRIVE_STRENGTH_4MA);
        gpio_set_slew_rate(data_pins[i], GPIO_SLEW_RATE_SLOW);
        gpio_set_input_hysteresis_enabled(data_pins[i], true);
    }

    s_write_dma_chan = dma_claim_unused_channel(true);
    s_read_dma_chan = dma_claim_unused_channel(true);

    s_write_dma_cfg = dma_channel_get_default_config(s_write_dma_chan);
    channel_config_set_transfer_data_size(&s_write_dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&s_write_dma_cfg, true);
    channel_config_set_write_increment(&s_write_dma_cfg, false);
    channel_config_set_dreq(&s_write_dma_cfg, pio_get_dreq(s_pio, s_sm, true));

    s_read_dma_cfg = dma_channel_get_default_config(s_read_dma_chan);
    channel_config_set_transfer_data_size(&s_read_dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&s_read_dma_cfg, false);
    channel_config_set_write_increment(&s_read_dma_cfg, true);
    channel_config_set_dreq(&s_read_dma_cfg, pio_get_dreq(s_pio, s_sm, false));

    mutex_init(&s_mutex);

    uint32_t sys_khz = clock_get_hz(clk_sys) / 1000;
    static const uint32_t tiers[] = { 50000, 25000 };
    for (unsigned t = 0; t < 2; t++) {
        s_target_khz = tiers[t];
        qpi_apply_timing(sys_khz);
        qpi_wake_chip();
        if (qpi_self_test()) {
            s_available = true;
            printf("[PIO_PSRAM_QPI] Initialised: %lu kHz SPI, chunks w=%lu r=%lu\n",
                   (unsigned long)s_spi_khz,
                   (unsigned long)s_chunk_write, (unsigned long)s_chunk_read);
            return true;
        }
        printf("[PIO_PSRAM_QPI] Tier %lu kHz failed\n", (unsigned long)tiers[t]);
    }

    // Full teardown so the serial fallback can claim everything.
    qpi_send_quad_cmd(PSRAM_CMD_EXIT_QPI);  // in case we are half-entered
    busy_wait_us(100);
    qpi_diag_read_id();
    pio_sm_set_enabled(s_pio, s_sm, false);
    dma_channel_unclaim(s_write_dma_chan);
    dma_channel_unclaim(s_read_dma_chan);
    pio_sm_unclaim(s_pio, s_sm);
    pio_remove_program(s_pio, &psram_qpi_program, s_prog_offs);
    gpio_deinit(PIO_PSRAM_PIN_SIO2);   // serial fallback re-inits GP2/GP3
    gpio_deinit(PIO_PSRAM_PIN_SIO3);
    s_sm = -1;
    s_write_dma_chan = -1;
    s_read_dma_chan = -1;
    s_target_khz = 0;
    s_spi_khz = 0;
    return false;
}

bool pio_psram_qpi_available(void) {
    return s_available;
}

void pio_psram_qpi_write(uint32_t addr, const uint8_t *src, uint32_t len) {
    if (!s_available || len == 0) return;
    if (is_cached_psram(src))
        src = flush_and_uncache(src);
    mutex_enter_blocking(&s_mutex);
    qpi_write_locked(addr, src, len);
    mutex_exit(&s_mutex);
}

void pio_psram_qpi_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    if (!s_available || len == 0) return;
    if (is_cached_psram(dst))
        dst = flush_and_uncache_dst(dst);
    mutex_enter_blocking(&s_mutex);
    qpi_read_locked(addr, dst, len);
    mutex_exit(&s_mutex);
}

void pio_psram_qpi_set_sysclk(uint32_t sys_khz) {
    if (!s_available) return;
    mutex_enter_blocking(&s_mutex);
    qpi_apply_timing(sys_khz);
    printf("[PIO_PSRAM_QPI] Retimed: %lu kHz SPI at %lu kHz sysclk\n",
           (unsigned long)s_spi_khz, (unsigned long)sys_khz);
    mutex_exit(&s_mutex);
}

uint32_t pio_psram_qpi_spi_khz(void) {
    return s_available ? s_spi_khz : 0;
}

void pio_psram_qpi_print_diag(void) {
    for (int i = 0; i < 2; i++) {
        if (!s_diag_fail[i].valid) continue;
        printf("[PSRAM] qpi self-test fail: tier=%lukHz addr=%06lX\n",
               (unsigned long)s_diag_fail[i].tier_khz,
               (unsigned long)s_diag_fail[i].addr);
        printf("[PSRAM]   wrote %02X %02X %02X %02X %02X %02X %02X %02X\n",
               s_diag_fail[i].wrote[0], s_diag_fail[i].wrote[1],
               s_diag_fail[i].wrote[2], s_diag_fail[i].wrote[3],
               s_diag_fail[i].wrote[4], s_diag_fail[i].wrote[5],
               s_diag_fail[i].wrote[6], s_diag_fail[i].wrote[7]);
        printf("[PSRAM]   got   %02X %02X %02X %02X %02X %02X %02X %02X\n",
               s_diag_fail[i].got[0], s_diag_fail[i].got[1],
               s_diag_fail[i].got[2], s_diag_fail[i].got[3],
               s_diag_fail[i].got[4], s_diag_fail[i].got[5],
               s_diag_fail[i].got[6], s_diag_fail[i].got[7]);
    }
    if (s_diag_readid_count > 0) {
        printf("[PSRAM] readid raw (%d bytes):", s_diag_readid_count);
        for (int i = 0; i < s_diag_readid_count; i++)
            printf(" %02X", s_diag_readid_raw[i]);
        printf("\n");
        // Decode: nibbles arrive high-first per byte; serial SO bit is bit1.
        uint16_t bits = 0;
        for (int i = 0; i < s_diag_readid_count && i < 8; i++) {
            uint8_t hi = (s_diag_readid_raw[i] >> 4) & 0xF;
            uint8_t lo = s_diag_readid_raw[i] & 0xF;
            bits = (uint16_t)((bits << 1) | ((hi >> 1) & 1));
            bits = (uint16_t)((bits << 1) | ((lo >> 1) & 1));
        }
        printf("[PSRAM] readid decoded: MF=%02X KGD=%02X (expect 0D 5D)\n",
               (bits >> 8) & 0xFF, bits & 0xFF);
    }
}
