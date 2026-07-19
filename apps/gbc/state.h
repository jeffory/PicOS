#pragma once

#include <stdint.h>
#include <stdbool.h>

struct PicoCalcAPI;

// Single-slot save states at /data/com.picos.gbc/states/<rom_name>.st
// File = gbc_state_header_t + raw gb_s blob + cart RAM.
//
// state.c treats the emulator state as an opaque blob: peanut_gb.h can
// only be included from one translation unit (it defines functions), so
// main.c owns sizeof(struct gb_s) and all pointer re-patching after load.
//
// gbc_state_load writes gb_state_out/cart_ram_out ONLY when the whole
// file was read and every header check passed. After a successful load
// the caller MUST re-patch every pointer field inside the gb blob —
// saved pointers may come from a previous run at a different load
// address.

bool gbc_state_save(const struct PicoCalcAPI *api, const char *rom_name,
                    const void *gb_state, uint32_t gb_state_size,
                    const uint8_t *cart_ram, uint32_t cart_ram_size,
                    const uint8_t *rom, uint32_t rom_size);

bool gbc_state_load(const struct PicoCalcAPI *api, const char *rom_name,
                    void *gb_state_out, uint32_t gb_state_size,
                    uint8_t *cart_ram_out, uint32_t cart_ram_size,
                    const uint8_t *rom, uint32_t rom_size);
