// pico/bootrom.h stub
#ifndef PICO_BOOTROM_H
#define PICO_BOOTROM_H

#include <stdint.h>
#include <stddef.h>

// Bootrom functions for flash manipulation (no-op on simulator)
static inline void rom_flash_range_erase(uint32_t flash_offs, size_t count, uint32_t block_size, uint8_t block_cmd) {
    (void)flash_offs;
    (void)count;
    (void)block_size;
    (void)block_cmd;
}

static inline void rom_flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count) {
    (void)flash_offs;
    (void)data;
    (void)count;
}

static inline void rom_flash_enter_cmd_xip(void) {}
static inline void rom_flash_exit_xip(void) {}
static inline void rom_flash_flush_cache(void) {}

// Bootrom table lookup (returns NULL on simulator)
static inline void* rom_func_lookup(uint32_t code) {
    (void)code;
    return NULL;
}

static inline void* rom_data_lookup(uint32_t code) {
    (void)code;
    return NULL;
}

// Helper macros for common bootrom functions
#define rom_func_lookup_inline(code) NULL

// Reset to USB boot (no-op)
static inline void reset_usb_boot(uint32_t usb_activity_gpio_pin_mask, uint32_t disable_interface_mask) {
    (void)usb_activity_gpio_pin_mask;
    (void)disable_interface_mask;
}

// Bootrom constants
#define ROM_TABLE_CODE(c1, c2) ((c1) | ((c2) << 8))

#endif // PICO_BOOTROM_H
