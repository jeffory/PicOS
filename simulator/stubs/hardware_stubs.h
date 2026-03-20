// Hardware Stubs Header
// Stub declarations for hardware-specific types and functions

#ifndef HARDWARE_STUBS_H
#define HARDWARE_STUBS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int uint;

// Hardware register bases (dummies)
extern void* io_bank0_base;
extern void* pads_bank0_base;

// DMA channel configuration (dummy)
struct dma_channel_config {
    uint32_t dummy;
};

// UART instance (dummy)
struct uart_inst {
    int dummy;
};

typedef struct uart_inst uart_inst_t;

// PIO instance (dummy)
struct pio_inst {
    int dummy;
};

typedef struct pio_inst PIO;

// PWM configuration (dummy)
struct pwm_config {
    uint32_t dummy;
};

// ADC (dummy)
static inline void adc_init(void) {}
static inline void adc_gpio_init(uint gpio) { (void)gpio; }
static inline void adc_select_input(uint input) { (void)input; }
static inline uint16_t adc_read(void) { return 0; }
static inline float adc_convert_temp(uint16_t adc_val) { (void)adc_val; return 25.0f; }

// UART stubs
static inline void uart_init(uart_inst_t* uart, uint baudrate) { (void)uart; (void)baudrate; }
static inline void uart_deinit(uart_inst_t* uart) { (void)uart; }
static inline void uart_set_baudrate(uart_inst_t* uart, uint baudrate) { (void)uart; (void)baudrate; }
static inline void uart_set_hw_flow(uart_inst_t* uart, bool cts, bool rts) { (void)uart; (void)cts; (void)rts; }
static inline void uart_set_format(uart_inst_t* uart, uint data_bits, uint stop_bits, uint parity) { 
    (void)uart; (void)data_bits; (void)stop_bits; (void)parity; 
}
static inline void uart_set_irq_enables(uart_inst_t* uart, bool rx, bool tx) { (void)uart; (void)rx; (void)tx; }
static inline bool uart_is_writable(uart_inst_t* uart) { (void)uart; return true; }
static inline bool uart_is_readable(uart_inst_t* uart) { (void)uart; return false; }
static inline void uart_putc_raw(uart_inst_t* uart, char c) { (void)uart; putchar(c); }
static inline void uart_putc(uart_inst_t* uart, char c) { (void)uart; putchar(c); }
static inline void uart_puts(uart_inst_t* uart, const char* s) { (void)uart; printf("%s", s); }
static inline char uart_getc(uart_inst_t* uart) { (void)uart; return 0; }
static inline void uart_default_tx_wait_blocking(void) {}

// Flash stubs
static inline void flash_range_erase(uint32_t flash_offs, size_t count) { (void)flash_offs; (void)count; }
static inline void flash_range_program(uint32_t flash_offs, const uint8_t* data, size_t count) { 
    (void)flash_offs; (void)data; (void)count; 
}
static inline void flash_get_unique_id(uint8_t* id_out) { 
    for (int i = 0; i < 8; i++) id_out[i] = 0; 
}

// Reset stubs
static inline void reset_cpu(void) { exit(0); }
static inline void reset_usb_boot(uint32_t usb_activity_gpio_pin_mask, uint32_t disable_interface_mask) { 
    (void)usb_activity_gpio_pin_mask; (void)disable_interface_mask; 
    exit(0); 
}

#endif // HARDWARE_STUBS_H
