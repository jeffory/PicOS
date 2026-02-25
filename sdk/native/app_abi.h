#pragma once
// =============================================================================
// PicOS Native App ABI — developer-facing header
//
// Include this in your app source alongside os.h:
//
//   #include "app_abi.h"
//   #include "os.h"
//
// Then implement picos_main():
//
//   void picos_main(const PicoCalcAPI *api,
//                   const char *app_dir,
//                   const char *app_id,
//                   const char *app_name)
//   {
//       api->display->clear(0);
//       api->display->drawText(10, 10, "Hello!", 0xFFFF, 0);
//       api->display->flush();
//       // ... wait for input, then return to exit
//   }
//
// Build with (see Makefile for the full command):
//   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -fpie -fno-plt
//       -T linker.ld -Wl,--entry=picos_main -Wl,-pie -nostartfiles
//       -o main.elf main.c
//
// TinyGo:
//   //export picos_main
//   func picoMain(api *C.PicoCalcAPI, dir, id, name *C.char) { ... }
//   tinygo build -target=picos -buildmode=pie -o main.elf .
// =============================================================================

#include <stdint.h>
#include <stdbool.h>

typedef struct PicoCalcAPI PicoCalcAPI;

// Native app entry point — export this symbol from your app.
typedef void (*picos_app_entry_t)(const PicoCalcAPI *api,
                                   const char        *app_dir,
                                   const char        *app_id,
                                   const char        *app_name);

// Optional context struct for CGo / TinyGo interop.
typedef struct {
    const PicoCalcAPI *api;
    const char        *app_dir;
    const char        *app_id;
    const char        *app_name;
} picos_app_context_t;
