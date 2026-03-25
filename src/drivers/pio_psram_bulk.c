#include "pio_psram_bulk.h"
#include "../hardware.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/xip_cache.h"
#include "pico/mutex.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

// XIP cache coherency — see pio_psram.c for detailed explanation.
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

// Include the generated PIO header
#include "pio_psram_bulk.pio.h"

// State
static PIO s_pio = NULL;
static int s_sm = -1;
static uint s_prog_offs = 0;
static bool s_available = false;
static mutex_t s_mutex;

// DMA channels
static int s_write_dma_chan = -1;
static int s_read_dma_chan = -1;
static dma_channel_config s_write_dma_cfg;
static dma_channel_config s_read_dma_cfg;

// Statistics
static pio_psram_bulk_stats_t s_stats;

// Forward declarations for command builders (used in self-test before definition)
static uint32_t build_write_cmd(uint8_t *buf, uint32_t addr, uint32_t data_len);
static uint32_t build_read_cmd(uint8_t *buf, uint32_t addr, uint32_t data_len);

// Transaction buffer for command assembly
// Format: [write_bits(16)][read_bits(16)][cmd(8)][addr(24)][data...]
// We need at least 8 bytes for the header
#define CMD_BUF_SIZE 12
static uint8_t s_cmd_buf[CMD_BUF_SIZE] __attribute__((aligned(4)));

bool pio_psram_bulk_init(void) {
    if (s_available) return true;

    printf("[PIO_PSRAM_BULK] Initialising on PIO1...\n");

    // Claim PIO1 state machine
    s_pio = PIO_PSRAM_PIO;
    s_prog_offs = pio_add_program(s_pio, &psram_bulk_fudge_program);
    s_sm = pio_claim_unused_sm(s_pio, true);
    if (s_sm < 0) {
        printf("[PIO_PSRAM_BULK] Failed to claim state machine\n");
        pio_remove_program(s_pio, &psram_bulk_fudge_program, s_prog_offs);
        return false;
    }

    // Initialize the state machine
    // clkdiv=1.0 at 200MHz sys_clk → 100MHz PIO clock → ~12.5MHz SPI
    psram_bulk_cs_init(s_pio, s_sm, s_prog_offs, 1.0f, true,
                       PIO_PSRAM_PIN_CS, PIO_PSRAM_PIN_MOSI, PIO_PSRAM_PIN_MISO);

    // Set up GPIO drive strength
    gpio_set_drive_strength(PIO_PSRAM_PIN_CS, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PIO_PSRAM_PIN_SCK, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PIO_PSRAM_PIN_MOSI, GPIO_DRIVE_STRENGTH_4MA);

    // Claim DMA channels
    s_write_dma_chan = dma_claim_unused_channel(true);
    s_read_dma_chan = dma_claim_unused_channel(true);

    // Configure write DMA (memory → PIO TX FIFO)
    s_write_dma_cfg = dma_channel_get_default_config(s_write_dma_chan);
    channel_config_set_transfer_data_size(&s_write_dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&s_write_dma_cfg, true);
    channel_config_set_write_increment(&s_write_dma_cfg, false);
    channel_config_set_dreq(&s_write_dma_cfg, pio_get_dreq(s_pio, s_sm, true));

    // Configure read DMA (PIO RX FIFO → memory)
    s_read_dma_cfg = dma_channel_get_default_config(s_read_dma_chan);
    channel_config_set_transfer_data_size(&s_read_dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&s_read_dma_cfg, false);
    channel_config_set_write_increment(&s_read_dma_cfg, true);
    channel_config_set_dreq(&s_read_dma_cfg, pio_get_dreq(s_pio, s_sm, false));

    // Initialize mutex for multi-core safety
    mutex_init(&s_mutex);

    // Self-test: write 4 bytes then read them back using direct PIO FIFO
    // access with timeouts.  We can't use the DMA-based bulk read/write
    // functions here because if the PSRAM chip is absent the read DMA hangs
    // forever (DREQ from an RX FIFO that never fills).
    {
        // --- Write 4 bytes at address 0 ---
        uint8_t wr_hdr[12];
        uint32_t wr_hdr_len = build_write_cmd(wr_hdr, 0, 4);
        // Append data
        wr_hdr[wr_hdr_len + 0] = 0xDE;
        wr_hdr[wr_hdr_len + 1] = 0xAD;
        wr_hdr[wr_hdr_len + 2] = 0xBE;
        wr_hdr[wr_hdr_len + 3] = 0xEF;
        uint32_t wr_total = wr_hdr_len + 4;

        // Push bytes into TX FIFO (PIO will shift them out on SPI)
        for (uint32_t i = 0; i < wr_total; i++)
            pio_sm_put_blocking(s_pio, s_sm, wr_hdr[i]);

        // Wait for TX FIFO to drain (write complete)
        absolute_time_t deadline = make_timeout_time_ms(100);
        while (!pio_sm_is_tx_fifo_empty(s_pio, s_sm)) {
            if (time_reached(deadline)) {
                printf("[PIO_PSRAM_BULK] Self-test FAILED (write timeout)\n");
                goto selftest_fail;
            }
            tight_loop_contents();
        }
        // Extra delay for PIO to clock out last byte
        sleep_us(100);

        // --- Read 4 bytes from address 0 ---
        // Drain any stale RX data
        while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm))
            (void)pio_sm_get(s_pio, s_sm);

        uint8_t rd_hdr[12];
        uint32_t rd_hdr_len = build_read_cmd(rd_hdr, 0, 4);

        for (uint32_t i = 0; i < rd_hdr_len; i++)
            pio_sm_put_blocking(s_pio, s_sm, rd_hdr[i]);

        // Collect 4 bytes from RX FIFO with timeout
        uint8_t readback[4] = {0};
        deadline = make_timeout_time_ms(100);
        for (int i = 0; i < 4; i++) {
            while (pio_sm_is_rx_fifo_empty(s_pio, s_sm)) {
                if (time_reached(deadline)) {
                    printf("[PIO_PSRAM_BULK] Self-test FAILED (read timeout, got %d/4 bytes)\n", i);
                    goto selftest_fail;
                }
                tight_loop_contents();
            }
            readback[i] = (uint8_t)pio_sm_get(s_pio, s_sm);
        }

        if (readback[0] != 0xDE || readback[1] != 0xAD ||
            readback[2] != 0xBE || readback[3] != 0xEF) {
            printf("[PIO_PSRAM_BULK] Self-test FAILED (read %02X%02X%02X%02X, expected DEADBEEF)\n",
                   readback[0], readback[1], readback[2], readback[3]);
            goto selftest_fail;
        }
    }

    goto selftest_pass;

selftest_fail:
    // Clean up all claimed resources so they don't leak
    pio_sm_set_enabled(s_pio, s_sm, false);
    dma_channel_unclaim(s_write_dma_chan);
    dma_channel_unclaim(s_read_dma_chan);
    pio_sm_unclaim(s_pio, s_sm);
    pio_remove_program(s_pio, &psram_bulk_fudge_program, s_prog_offs);
    s_sm = -1;
    s_write_dma_chan = -1;
    s_read_dma_chan = -1;
    return false;

selftest_pass:
    printf("[PIO_PSRAM_BULK] Initialised: max %d bytes/write, %d bytes/read\n",
           PIO_PSRAM_BULK_MAX_WRITE, PIO_PSRAM_BULK_MAX_READ);
    return true;
}

bool pio_psram_bulk_available(void) {
    return s_available;
}

// Build a write command buffer
// Returns total bytes to send (header + data)
static uint32_t build_write_cmd(uint8_t *buf, uint32_t addr, uint32_t data_len) {
    // write_bits = 32 (cmd+addr) + data_len*8
    uint32_t write_bits = 32 + data_len * 8;
    uint32_t read_bits = 0;
    
    // Little-endian 16-bit counters
    buf[0] = write_bits & 0xFF;
    buf[1] = (write_bits >> 8) & 0xFF;
    buf[2] = read_bits & 0xFF;
    buf[3] = (read_bits >> 8) & 0xFF;
    
    // Command and address
    buf[4] = 0x02;  // Write command
    buf[5] = (addr >> 16) & 0xFF;
    buf[6] = (addr >> 8) & 0xFF;
    buf[7] = addr & 0xFF;
    
    return 8;  // Header size
}

// Build a read command buffer
// Returns header bytes to send
static uint32_t build_read_cmd(uint8_t *buf, uint32_t addr, uint32_t data_len) {
    // write_bits = 40 (cmd+addr+dummy)
    uint32_t write_bits = 40;
    uint32_t read_bits = data_len * 8;
    
    buf[0] = write_bits & 0xFF;
    buf[1] = (write_bits >> 8) & 0xFF;
    buf[2] = read_bits & 0xFF;
    buf[3] = (read_bits >> 8) & 0xFF;
    
    buf[4] = 0x0B;  // Fast read command
    buf[5] = (addr >> 16) & 0xFF;
    buf[6] = (addr >> 8) & 0xFF;
    buf[7] = addr & 0xFF;
    buf[8] = 0;     // Dummy byte
    
    return 9;  // Header size
}

void pio_psram_bulk_write(uint32_t addr, const uint8_t *src, uint32_t len) {
    if (!s_available || len == 0) return;
    if (len > PIO_PSRAM_BULK_MAX_WRITE) {
        pio_psram_bulk_write_large(addr, src, len);
        return;
    }
    if (is_cached_psram(src))
        src = flush_and_uncache(src);

    mutex_enter_blocking(&s_mutex);

    // Build command header
    uint32_t hdr_len = build_write_cmd(s_cmd_buf, addr, len);

    // Send header via DMA
    dma_channel_configure(s_write_dma_chan, &s_write_dma_cfg,
                          &s_pio->txf[s_sm],  // Write to PIO TX FIFO
                          s_cmd_buf,           // Read from command buffer
                          hdr_len,             // Transfer count
                          true);               // Start immediately
    dma_channel_wait_for_finish_blocking(s_write_dma_chan);

    // Send data via DMA
    dma_channel_configure(s_write_dma_chan, &s_write_dma_cfg,
                          &s_pio->txf[s_sm],
                          src,
                          len,
                          true);
    dma_channel_wait_for_finish_blocking(s_write_dma_chan);

    s_stats.write_calls++;
    s_stats.write_bytes += len;
    s_stats.write_chunks += 2;  // header + data

    mutex_exit(&s_mutex);
}

void pio_psram_bulk_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    if (!s_available || len == 0) return;
    if (len > PIO_PSRAM_BULK_MAX_READ) {
        pio_psram_bulk_read_large(addr, dst, len);
        return;
    }
    if (is_cached_psram(dst))
        dst = flush_and_uncache_dst(dst);

    mutex_enter_blocking(&s_mutex);

    // Build command header
    uint32_t hdr_len = build_read_cmd(s_cmd_buf, addr, len);

    // Start read DMA (will capture data from PIO RX FIFO)
    dma_channel_configure(s_read_dma_chan, &s_read_dma_cfg,
                          dst,                 // Write to destination
                          &s_pio->rxf[s_sm],    // Read from PIO RX FIFO
                          len,                  // Transfer count
                          true);                // Start immediately

    // Send header via write DMA
    dma_channel_configure(s_write_dma_chan, &s_write_dma_cfg,
                          &s_pio->txf[s_sm],
                          s_cmd_buf,
                          hdr_len,
                          true);
    dma_channel_wait_for_finish_blocking(s_write_dma_chan);

    // Wait for read DMA to complete
    dma_channel_wait_for_finish_blocking(s_read_dma_chan);

    s_stats.read_calls++;
    s_stats.read_bytes += len;
    s_stats.read_chunks += 2;

    mutex_exit(&s_mutex);
}

void pio_psram_bulk_write_large(uint32_t addr, const uint8_t *src, uint32_t len) {
    // Flush cache once for the entire large transfer; inner calls get uncached ptr
    if (is_cached_psram(src))
        src = flush_and_uncache(src);
    while (len > 0) {
        uint32_t chunk = (len > PIO_PSRAM_BULK_MAX_WRITE) ? PIO_PSRAM_BULK_MAX_WRITE : len;
        pio_psram_bulk_write(addr, src, chunk);
        addr += chunk;
        src += chunk;
        len -= chunk;
    }
}

void pio_psram_bulk_read_large(uint32_t addr, uint8_t *dst, uint32_t len) {
    // Flush cache once for the entire large transfer; inner calls get uncached ptr
    if (is_cached_psram(dst))
        dst = flush_and_uncache_dst(dst);
    while (len > 0) {
        uint32_t chunk = (len > PIO_PSRAM_BULK_MAX_READ) ? PIO_PSRAM_BULK_MAX_READ : len;
        pio_psram_bulk_read(addr, dst, chunk);
        addr += chunk;
        dst += chunk;
        len -= chunk;
    }
}

void pio_psram_bulk_get_stats(pio_psram_bulk_stats_t *stats) {
    if (stats) *stats = s_stats;
}

void pio_psram_bulk_reset_stats(void) {
    memset(&s_stats, 0, sizeof(s_stats));
}
