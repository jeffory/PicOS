// hardware/gpio.h stub
#ifndef HARDWARE_GPIO_H
#define HARDWARE_GPIO_H

#include <stdint.h>
#include <stdbool.h>

// GPIO pin functions
enum gpio_function {
    GPIO_FUNC_XIP = 0,
    GPIO_FUNC_SPI,
    GPIO_FUNC_UART,
    GPIO_FUNC_I2C,
    GPIO_FUNC_PWM,
    GPIO_FUNC_SIO,
    GPIO_FUNC_PIO0,
    GPIO_FUNC_PIO1,
    GPIO_FUNC_GPCK,
    GPIO_FUNC_USB,
    GPIO_FUNC_NULL = 0x1f,
};

// GPIO direction
static inline void gpio_init(uint gpio) { (void)gpio; }
static inline void gpio_deinit(uint gpio) { (void)gpio; }
static inline void gpio_set_dir(uint gpio, bool out) { (void)gpio; (void)out; }
static inline void gpio_set_dir_in_masked(uint32_t mask) { (void)mask; }
static inline void gpio_set_dir_out_masked(uint32_t mask) { (void)mask; }
static inline void gpio_put(uint gpio, int value) { (void)gpio; (void)value; }
static inline bool gpio_get(uint gpio) { (void)gpio; return false; }
static inline void gpio_set_pulls(uint gpio, bool up, bool down) { (void)gpio; (void)up; (void)down; }
static inline void gpio_pull_up(uint gpio) { (void)gpio; }
static inline void gpio_pull_down(uint gpio) { (void)gpio; }
static inline void gpio_disable_pulls(uint gpio) { (void)gpio; }
static inline void gpio_set_function(uint gpio, uint32_t fn) { (void)gpio; (void)fn; }
static inline uint32_t gpio_get_function(uint gpio) { (void)gpio; return GPIO_FUNC_NULL; }
static inline void gpio_set_irq_enabled(uint gpio, uint32_t events, bool enabled) { 
    (void)gpio; (void)events; (void)enabled; 
}
static inline void gpio_set_irq_enabled_with_callback(uint gpio, uint32_t events, bool enabled, void (*callback)(void)) {
    (void)gpio; (void)events; (void)enabled; (void)callback;
}
static inline void gpio_add_raw_irq_handler_with_args(uint gpio, void (*handler)(void), void *args) {
    (void)gpio; (void)handler; (void)args;
}
static inline void gpio_set_input_enabled(uint gpio, bool enabled) { (void)gpio; (void)enabled; }
static inline void gpio_set_input_hysteresis_enabled(uint gpio, bool enabled) { (void)gpio; (void)enabled; }
static inline void gpio_set_slew_rate(uint gpio, uint32_t slew) { (void)gpio; (void)slew; }
static inline void gpio_set_drive_strength(uint gpio, uint32_t drive) { (void)gpio; (void)drive; }
static inline void gpio_set_overdrive(uint gpio, bool overdrive) { (void)gpio; (void)overdrive; }

// GPIO mask operations
static inline void gpio_put_masked(uint32_t mask, uint32_t value) { (void)mask; (void)value; }
static inline void gpio_put_all(uint32_t value) { (void)value; }
static inline uint32_t gpio_get_all(void) { return 0; }

// IRQ events
#define GPIO_IRQ_LEVEL_LOW  0x1u
#define GPIO_IRQ_LEVEL_HIGH 0x2u
#define GPIO_IRQ_EDGE_FALL  0x4u
#define GPIO_IRQ_EDGE_RISE  0x8u

#endif // HARDWARE_GPIO_H
