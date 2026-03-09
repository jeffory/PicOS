/******************************************************************************

rp2040-psram

Copyright © 2023 Ian Scott

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

******************************************************************************/

/**
 * @file psram_spi.h
 *
 * The following defines MUST be set via compile definitions before including:
 *   PSRAM_PIN_CS, PSRAM_PIN_SCK, PSRAM_PIN_MOSI, PSRAM_PIN_MISO
 *
 * Optional: PSRAM_MUTEX (for multi-core safety), PSRAM_ASYNC
 *
 * Project homepage: https://github.com/polpo/rp2040-psram
 */

#pragma once

#ifndef PSRAM_PIN_CS
#error "PSRAM_PIN_CS must be defined"
#endif
#ifndef PSRAM_PIN_SCK
#error "PSRAM_PIN_SCK must be defined"
#endif
#ifndef PSRAM_PIN_MOSI
#error "PSRAM_PIN_MOSI must be defined"
#endif
#ifndef PSRAM_PIN_MISO
#error "PSRAM_PIN_MISO must be defined"
#endif

#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/dma.h"
#if defined(PSRAM_MUTEX)
#include "pico/mutex.h"
#elif defined(PSRAM_SPINLOCK)
#include "hardware/sync.h"
#endif
#include <string.h>

#include "psram_spi.pio.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct psram_spi_inst
    {
        PIO pio;
        int sm;
        uint offset;
#if defined(PSRAM_MUTEX)
        mutex_t mtx;
#elif defined(PSRAM_SPINLOCK)
    spin_lock_t *spinlock;
    uint32_t spin_irq_state;
#endif
        int write_dma_chan;
        dma_channel_config write_dma_chan_config;
        int read_dma_chan;
        dma_channel_config read_dma_chan_config;
#if defined(PSRAM_ASYNC)
        int async_dma_chan;
        dma_channel_config async_dma_chan_config;
#endif
    } psram_spi_inst_t;

#if defined(PSRAM_ASYNC)
    extern psram_spi_inst_t *async_spi_inst;
#endif

    __force_inline static void __time_critical_func(pio_spi_write_read_blocking)(
        psram_spi_inst_t *spi,
        const uint8_t *src, const size_t src_len,
        uint8_t *dst, const size_t dst_len)
    {
        size_t tx_remain = src_len, rx_remain = dst_len;

#if defined(PSRAM_MUTEX)
        mutex_enter_blocking(&spi->mtx);
#elif defined(PSRAM_SPINLOCK)
    spi->spin_irq_state = spin_lock_blocking(spi->spinlock);
#endif
        io_rw_8 *txfifo = (io_rw_8 *)&spi->pio->txf[spi->sm];
        while (tx_remain)
        {
            if (!pio_sm_is_tx_fifo_full(spi->pio, spi->sm))
            {
                *txfifo = *src++;
                --tx_remain;
            }
        }

        io_rw_8 *rxfifo = (io_rw_8 *)&spi->pio->rxf[spi->sm];
        while (rx_remain)
        {
            if (!pio_sm_is_rx_fifo_empty(spi->pio, spi->sm))
            {
                *dst++ = *rxfifo;
                --rx_remain;
            }
        }

#if defined(PSRAM_MUTEX)
        mutex_exit(&spi->mtx);
#elif defined(PSRAM_SPINLOCK)
    spin_unlock(spi->spinlock, spi->spin_irq_state);
#endif
    }

    __force_inline static void __time_critical_func(pio_spi_write_dma_blocking)(
        psram_spi_inst_t *spi,
        const uint8_t *src, const size_t src_len)
    {
#ifdef PSRAM_MUTEX
        mutex_enter_blocking(&spi->mtx);
#elif defined(PSRAM_SPINLOCK)
    spi->spin_irq_state = spin_lock_blocking(spi->spinlock);
#endif
        dma_channel_transfer_from_buffer_now(spi->write_dma_chan, src, src_len);
        dma_channel_wait_for_finish_blocking(spi->write_dma_chan);
#ifdef PSRAM_MUTEX
        mutex_exit(&spi->mtx);
#elif defined(PSRAM_SPINLOCK)
    spin_unlock(spi->spinlock, spi->spin_irq_state);
#endif
    }

    __force_inline static void __time_critical_func(pio_spi_write_read_dma_blocking)(
        psram_spi_inst_t *spi,
        const uint8_t *src, const size_t src_len,
        uint8_t *dst, const size_t dst_len)
    {
#ifdef PSRAM_MUTEX
        mutex_enter_blocking(&spi->mtx);
#elif defined(PSRAM_SPINLOCK)
    spi->spin_irq_state = spin_lock_blocking(spi->spinlock);
#endif
        dma_channel_transfer_from_buffer_now(spi->write_dma_chan, src, src_len);
        dma_channel_transfer_to_buffer_now(spi->read_dma_chan, dst, dst_len);
        dma_channel_wait_for_finish_blocking(spi->write_dma_chan);
        dma_channel_wait_for_finish_blocking(spi->read_dma_chan);
#ifdef PSRAM_MUTEX
        mutex_exit(&spi->mtx);
#elif defined(PSRAM_SPINLOCK)
    spin_unlock(spi->spinlock, spi->spin_irq_state);
#endif
    }

    psram_spi_inst_t psram_spi_init_clkdiv(PIO pio, int sm, float clkdiv, bool fudge);
    psram_spi_inst_t psram_spi_init(PIO pio, int sm);
    int test_psram(psram_spi_inst_t *psram_spi, int increment);
    void psram_spi_uninit(psram_spi_inst_t spi, bool fudge);

    static uint8_t write8_command[] = {
        40,      // 40 bits write
        0,       // 0 bits read
        0x02u,   // Write command
        0, 0, 0, // Address
        0        // 8 bits data
    };

    __force_inline static void psram_write8(psram_spi_inst_t *spi, uint32_t addr, uint8_t val)
    {
        write8_command[3] = addr >> 16;
        write8_command[4] = addr >> 8;
        write8_command[5] = addr;
        write8_command[6] = val;

        pio_spi_write_dma_blocking(spi, write8_command, sizeof(write8_command));
    };

    static uint8_t read8_command[] = {
        40,      // 40 bits write
        8,       // 8 bits read
        0x0bu,   // Fast read command
        0, 0, 0, // Address
        0        // 8 delay cycles
    };

    __force_inline static uint8_t psram_read8(psram_spi_inst_t *spi, uint32_t addr)
    {
        read8_command[3] = addr >> 16;
        read8_command[4] = addr >> 8;
        read8_command[5] = addr;

        uint8_t val;
        pio_spi_write_read_dma_blocking(spi, read8_command, sizeof(read8_command), &val, 1);
        return val;
    };

    static uint8_t write16_command[] = {
        48,      // 48 bits write
        0,       // 0 bits read
        0x02u,   // Write command
        0, 0, 0, // Address
        0, 0     // 16 bits data
    };

    __force_inline static void psram_write16(psram_spi_inst_t *spi, uint32_t addr, uint16_t val)
    {
        write16_command[3] = addr >> 16;
        write16_command[4] = addr >> 8;
        write16_command[5] = addr;
        write16_command[6] = val;
        write16_command[7] = val >> 8;

        pio_spi_write_dma_blocking(spi, write16_command, sizeof(write16_command));
    };

    static uint8_t read16_command[] = {
        40,      // 40 bits write
        16,      // 16 bits read
        0x0bu,   // Fast read command
        0, 0, 0, // Address
        0        // 8 delay cycles
    };

    __force_inline static uint16_t psram_read16(psram_spi_inst_t *spi, uint32_t addr)
    {
        read16_command[3] = addr >> 16;
        read16_command[4] = addr >> 8;
        read16_command[5] = addr;

        uint16_t val;
        pio_spi_write_read_dma_blocking(spi, read16_command, sizeof(read16_command), (unsigned char *)&val, 2);
        return val;
    };

    static uint8_t write32_command[] = {
        64,        // 64 bits write
        0,         // 0 bits read
        0x02u,     // Write command
        0, 0, 0,   // Address
        0, 0, 0, 0 // 32 bits data
    };

    __force_inline static void psram_write32(psram_spi_inst_t *spi, uint32_t addr, uint32_t val)
    {
        write32_command[3] = addr >> 16;
        write32_command[4] = addr >> 8;
        write32_command[5] = addr;
        write32_command[6] = val;
        write32_command[7] = val >> 8;
        write32_command[8] = val >> 16;
        write32_command[9] = val >> 24;

        pio_spi_write_dma_blocking(spi, write32_command, sizeof(write32_command));
    };

    static uint8_t read32_command[] = {
        40,      // 40 bits write
        32,      // 32 bits read
        0x0bu,   // Fast read command
        0, 0, 0, // Address
        0        // 8 delay cycles
    };

    __force_inline static uint32_t psram_read32(psram_spi_inst_t *spi, uint32_t addr)
    {
        read32_command[3] = addr >> 16;
        read32_command[4] = addr >> 8;
        read32_command[5] = addr;

        uint32_t val;
        pio_spi_write_read_dma_blocking(spi, read32_command, sizeof(read32_command), (unsigned char *)&val, 4);
        return val;
    };

    static uint8_t write_command[] = {
        0,      // n bits write
        0,      // 0 bits read
        0x02u,  // Fast write command
        0, 0, 0 // Address
    };

    __force_inline static void psram_write(psram_spi_inst_t *spi, const uint32_t addr, const uint8_t *src, const size_t count)
    {
        write_command[0] = (4 + count) * 8;
        write_command[3] = addr >> 16;
        write_command[4] = addr >> 8;
        write_command[5] = addr;

        pio_spi_write_dma_blocking(spi, write_command, sizeof(write_command));
        pio_spi_write_dma_blocking(spi, src, count);
    };

    static uint8_t read_command[] = {
        40,      // 40 bits write
        0,       // n bits read
        0x0bu,   // Fast read command
        0, 0, 0, // Address
        0        // 8 delay cycles
    };

    __force_inline static void psram_read(psram_spi_inst_t *spi, const uint32_t addr, uint8_t *dst, const size_t count)
    {
        read_command[1] = count * 8;
        read_command[3] = addr >> 16;
        read_command[4] = addr >> 8;
        read_command[5] = addr;

        pio_spi_write_read_dma_blocking(spi, read_command, sizeof(read_command), dst, count);
    };

#ifdef __cplusplus
}
#endif
