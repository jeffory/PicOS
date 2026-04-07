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

// Write a byte to the PIO TX FIFO using an 8-bit store.
// On the AHB bus a narrow write is replicated across the full 32-bit word,
// so the byte lands in every lane and the PIO's MSB-first shift reads it
// correctly.  This matches how the polpo library and DMA_SIZE_8 feed data.
static inline void pio_put_byte(uint8_t val) {
    while (pio_sm_is_tx_fifo_full(s_pio, s_sm))
        tight_loop_contents();
    *(volatile uint8_t *)&s_pio->txf[s_sm] = val;
}

// Send a simple PSRAM command (reset etc).
// Protocol: [write_bits=8, read_bits=0, cmd] → enters streaming write mode,
// then [0] terminates the stream and deasserts CS.
static void send_simple_cmd(uint8_t cmd) {
    pio_put_byte(8);     // 8 write bits
    pio_put_byte(0);     // 0 read bits → streaming write mode
    pio_put_byte(cmd);   // command byte
    pio_put_byte(0);     // streaming terminator → CS deasserts
    while (!pio_sm_is_tx_fifo_empty(s_pio, s_sm))
        tight_loop_contents();
}

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
    // clkdiv=4.0 at 200MHz sys_clk -> 50MHz PIO clock -> 25MHz SPI
    // At clkdiv=2.0 (50MHz SPI), MISO has only 10ns to settle vs PSRAM tCLQV
    // of ~8ns, leaving <2ns margin. clkdiv=4.0 gives 20ns → 12ns margin.
    psram_bulk_cs_init(s_pio, s_sm, s_prog_offs, 4.0f,
                       PIO_PSRAM_PIN_CS, PIO_PSRAM_PIN_MOSI, PIO_PSRAM_PIN_MISO);

    // Set up GPIO drive strength and slew rate
    gpio_set_drive_strength(PIO_PSRAM_PIN_CS, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PIO_PSRAM_PIN_SCK, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_drive_strength(PIO_PSRAM_PIN_MOSI, GPIO_DRIVE_STRENGTH_4MA);
    gpio_set_slew_rate(PIO_PSRAM_PIN_CS, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIO_PSRAM_PIN_SCK, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(PIO_PSRAM_PIN_MOSI, GPIO_SLEW_RATE_FAST);

    // PSRAM reset sequence (required for reliable operation)
    send_simple_cmd(0x66);   // Reset Enable
    busy_wait_us(50);
    send_simple_cmd(0x99);   // Reset
    busy_wait_us(100);

    // Claim DMA channels
    s_write_dma_chan = dma_claim_unused_channel(true);
    s_read_dma_chan = dma_claim_unused_channel(true);

    // Configure write DMA (memory -> PIO TX FIFO)
    s_write_dma_cfg = dma_channel_get_default_config(s_write_dma_chan);
    channel_config_set_transfer_data_size(&s_write_dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&s_write_dma_cfg, true);
    channel_config_set_write_increment(&s_write_dma_cfg, false);
    channel_config_set_dreq(&s_write_dma_cfg, pio_get_dreq(s_pio, s_sm, true));

    // Configure read DMA (PIO RX FIFO -> memory)
    s_read_dma_cfg = dma_channel_get_default_config(s_read_dma_chan);
    channel_config_set_transfer_data_size(&s_read_dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&s_read_dma_cfg, false);
    channel_config_set_write_increment(&s_read_dma_cfg, true);
    channel_config_set_dreq(&s_read_dma_cfg, pio_get_dreq(s_pio, s_sm, false));

    // Initialize mutex for multi-core safety
    mutex_init(&s_mutex);

    // Self-test: write 4 bytes then read them back
    // Uses polpo-compatible protocol (proven timing)
    {
        // --- Write 4 bytes at address 0 ---
        // [write_bits=64, read_bits=0, cmd=0x02, addr, data...] [0=end stream]
        uint8_t wr_cmd[] = { 64, 0, 0x02, 0x00, 0x00, 0x00,
                             0xDE, 0xAD, 0xBE, 0xEF,
                             0 };  // streaming terminator
        for (uint32_t i = 0; i < sizeof(wr_cmd); i++)
            pio_put_byte(wr_cmd[i]);

        absolute_time_t deadline = make_timeout_time_ms(100);
        while (!pio_sm_is_tx_fifo_empty(s_pio, s_sm)) {
            if (time_reached(deadline)) {
                printf("[PIO_PSRAM_BULK] Self-test FAILED (write timeout)\n");
                goto selftest_fail;
            }
            tight_loop_contents();
        }
        sleep_us(100);

        // --- Read 4 bytes from address 0 ---
        // [write_bits=40, read_bits=32, cmd=0x0B, addr, dummy]
        while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm))
            (void)pio_sm_get(s_pio, s_sm);

        uint8_t rd_cmd[] = { 40, 31, 0x0B, 0x00, 0x00, 0x00, 0x00 };
        for (uint32_t i = 0; i < sizeof(rd_cmd); i++)
            pio_put_byte(rd_cmd[i]);

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
    s_available = true;
    printf("[PIO_PSRAM_BULK] Initialised (%d/%d bytes per chunk)\n",
           PIO_PSRAM_BULK_CHUNK_WRITE, PIO_PSRAM_BULK_CHUNK_READ);
    return true;
}

bool pio_psram_bulk_available(void) {
    return s_available;
}

void pio_psram_bulk_write(uint32_t addr, const uint8_t *src, uint32_t len) {
    if (!s_available || len == 0) return;
    if (is_cached_psram(src))
        src = flush_and_uncache(src);

    mutex_enter_blocking(&s_mutex);

    // Per-transaction writes: each chunk is a complete SPI transaction
    // with its own cmd+addr, matching the self-test protocol.
    // [write_bits=(4+chunk)*8, read_bits=0, cmd, addr, data...] [0=end]
    uint32_t remaining = len;
    const uint8_t *p = src;
    while (remaining > 0) {
        uint32_t chunk = (remaining > PIO_PSRAM_BULK_CHUNK_WRITE)
                         ? PIO_PSRAM_BULK_CHUNK_WRITE : remaining;

        pio_put_byte((4 + chunk) * 8);  // write_bits for cmd+addr+data
        pio_put_byte(0);                // read_bits = 0 → streaming mode
        pio_put_byte(0x02);             // write command
        pio_put_byte((addr >> 16) & 0xFF);
        pio_put_byte((addr >> 8) & 0xFF);
        pio_put_byte(addr & 0xFF);

        dma_channel_configure(s_write_dma_chan, &s_write_dma_cfg,
                              &s_pio->txf[s_sm], p, chunk, true);
        dma_channel_wait_for_finish_blocking(s_write_dma_chan);

        // Streaming terminator → CS deasserts
        pio_put_byte(0);

        while (!pio_sm_is_tx_fifo_empty(s_pio, s_sm))
            tight_loop_contents();

        addr += chunk;
        p += chunk;
        remaining -= chunk;
    }

    sleep_us(1);

    s_stats.write_calls++;
    s_stats.write_bytes += len;

    mutex_exit(&s_mutex);
}

void pio_psram_bulk_read(uint32_t addr, uint8_t *dst, uint32_t len) {
    if (!s_available || len == 0) return;
    if (is_cached_psram(dst))
        dst = flush_and_uncache_dst(dst);

    mutex_enter_blocking(&s_mutex);

    // Non-streaming reads: each chunk is a separate polpo-compatible transaction.
    // This uses the proven fudge read timing (no chunk boundary issues).
    uint32_t remaining = len;
    uint8_t *p = dst;
    while (remaining > 0) {
        uint32_t chunk = (remaining > PIO_PSRAM_BULK_CHUNK_READ)
                         ? PIO_PSRAM_BULK_CHUNK_READ : remaining;

        // Drain stale RX data
        while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm))
            (void)pio_sm_get(s_pio, s_sm);

        // [write_bits=40, read_bits=chunk*8-1, cmd, addr, dummy]
        // read_bits is chunk*8-1 because PIO enters readloop (not readloop_mid),
        // reading one extra bit vs the y counter value.
        pio_put_byte(40);
        pio_put_byte(chunk * 8 - 1);
        pio_put_byte(0x0B);
        pio_put_byte((addr >> 16) & 0xFF);
        pio_put_byte((addr >> 8) & 0xFF);
        pio_put_byte(addr & 0xFF);
        pio_put_byte(0x00);  // dummy byte

        // DMA capture
        dma_channel_configure(s_read_dma_chan, &s_read_dma_cfg,
                              p, &s_pio->rxf[s_sm], chunk, true);
        dma_channel_wait_for_finish_blocking(s_read_dma_chan);

        addr += chunk;
        p += chunk;
        remaining -= chunk;
    }

    s_stats.read_calls++;
    s_stats.read_bytes += len;

    mutex_exit(&s_mutex);
}

void pio_psram_bulk_get_stats(pio_psram_bulk_stats_t *stats) {
    if (stats) *stats = s_stats;
}

void pio_psram_bulk_reset_stats(void) {
    memset(&s_stats, 0, sizeof(s_stats));
}
