// hardware/clocks.h stub
#ifndef HARDWARE_CLOCKS_H
#define HARDWARE_CLOCKS_H

#include <stdint.h>
#include <stdbool.h>

// Clock sources
enum clock_index {
    clk_gpout0 = 0,
    clk_gpout1,
    clk_gpout2,
    clk_gpout3,
    clk_ref,
    clk_sys,
    clk_peri,
    clk_usb,
    clk_adc,
    clk_rtc,
    CLK_COUNT
};

// Clock functions
static inline uint32_t clock_get_hz(int clk_index) {
    (void)clk_index;
    return 125000000; // Return 125 MHz as default
}

static inline uint32_t frequency_count_khz(unsigned int src) {
    (void)src;
    return 125000; // 125 MHz in kHz
}

static inline void clocks_init(void) {}
static inline void set_sys_clock_48mhz(void) {}
static inline bool set_sys_clock_khz(uint32_t freq_khz, bool required) {
    (void)freq_khz;
    (void)required;
    return true;
}

// Clock GPIO output (no-op)
static inline void clock_gpio_init_int_frac(unsigned int gpio, unsigned int src, uint32_t div_int, uint8_t div_frac) {
    (void)gpio;
    (void)src;
    (void)div_int;
    (void)div_frac;
}

static inline void clock_gpio_init(unsigned int gpio, unsigned int src, uint32_t div) {
    (void)gpio;
    (void)src;
    (void)div;
}

static inline void clock_configure(int clk_index, uint32_t src, uint32_t auxsrc, uint32_t src_freq, uint32_t freq) {
    (void)clk_index;
    (void)src;
    (void)auxsrc;
    (void)src_freq;
    (void)freq;
}

static inline void clock_stop(int clk_index) {
    (void)clk_index;
}

// Clock source values
#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS 1
#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB 2
#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_ROSC_CLKSRC_PH 3
#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_XOSC_CLKSRC    4
#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_GPIN0   5
#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_GPIN1   6

#endif // HARDWARE_CLOCKS_H
