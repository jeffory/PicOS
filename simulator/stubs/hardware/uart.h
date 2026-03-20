// hardware/uart.h stub
#ifndef HARDWARE_UART_H
#define HARDWARE_UART_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// UART instance type
typedef struct uart_inst uart_inst_t;

// Default UART instances
extern uart_inst_t* uart0;
extern uart_inst_t* uart1;

// UART functions
static inline void uart_init(uart_inst_t* uart, uint32_t baudrate) {
    (void)uart;
    (void)baudrate;
}

static inline void uart_deinit(uart_inst_t* uart) {
    (void)uart;
}

static inline void uart_set_baudrate(uart_inst_t* uart, uint32_t baudrate) {
    (void)uart;
    (void)baudrate;
}

static inline void uart_set_hw_flow(uart_inst_t* uart, bool cts, bool rts) {
    (void)uart;
    (void)cts;
    (void)rts;
}

static inline void uart_set_format(uart_inst_t* uart, uint32_t data_bits, uint32_t stop_bits, uint32_t parity) {
    (void)uart;
    (void)data_bits;
    (void)stop_bits;
    (void)parity;
}

static inline void uart_set_irq_enables(uart_inst_t* uart, bool rx, bool tx) {
    (void)uart;
    (void)rx;
    (void)tx;
}

static inline bool uart_is_writable(uart_inst_t* uart) {
    (void)uart;
    return true;
}

static inline bool uart_is_readable(uart_inst_t* uart) {
    (void)uart;
    return false;
}

static inline void uart_putc_raw(uart_inst_t* uart, char c) {
    (void)uart;
    putchar(c);
}

static inline void uart_putc(uart_inst_t* uart, char c) {
    (void)uart;
    putchar(c);
}

static inline void uart_puts(uart_inst_t* uart, const char* s) {
    (void)uart;
    printf("%s", s);
}

static inline char uart_getc(uart_inst_t* uart) {
    (void)uart;
    return 0;
}

static inline void uart_default_tx_wait_blocking(void) {}

// Get default UART instance
static inline uart_inst_t* uart_get_instance(uint32_t num) {
    (void)num;
    return NULL;
}

// TX/RX FIFO levels
static inline uint32_t uart_get_index(uart_inst_t* uart) {
    (void)uart;
    return 0;
}

static inline size_t uart_get_tx_buffer_size(uart_inst_t* uart) {
    (void)uart;
    return 32;
}

static inline size_t uart_get_rx_buffer_size(uart_inst_t* uart) {
    (void)uart;
    return 32;
}

#endif // HARDWARE_UART_H
