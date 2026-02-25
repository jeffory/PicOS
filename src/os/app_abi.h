#pragma once
#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// PicOS Native App ABI
//
// Native (C / TinyGo) apps are Position-Independent ELF32 binaries placed at
// /apps/<name>/main.elf on the SD card.  The OS loads them into PSRAM at
// runtime and calls the entry point below.
//
// Symbol name:  picos_main   (set via --entry=picos_main in the linker)
// Calling conv: ARM AAPCS (C calling convention)
//
// TinyGo: export as //export picos_main from the package main function.
// =============================================================================

// Forward declaration — app_abi.h is dependency-free; include os.h for the
// full PicoCalcAPI definition.
typedef struct PicoCalcAPI PicoCalcAPI;

// Primary entry point function type for native apps.
typedef void (*picos_app_entry_t)(const PicoCalcAPI *api,
                                   const char        *app_dir,
                                   const char        *app_id,
                                   const char        *app_name);

// Optional single-struct variant for CGo / TinyGo interop.
typedef struct {
    const PicoCalcAPI *api;
    const char        *app_dir;
    const char        *app_id;
    const char        *app_name;
} picos_app_context_t;
