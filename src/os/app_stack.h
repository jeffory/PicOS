#pragma once

// =============================================================================
// App stack: run an app runtime on its own Process Stack (PSP)
//
// Core 0's main stack (MSP) is only 4 KB (SCRATCH_Y) and also takes every
// interrupt. Both app runtimes therefore run in Thread mode on the PSP, on a
// stack the runner allocates at launch:
//   - native ELF apps (native_loader.c): the app's entry point
//   - Lua apps (lua_runner.c): the whole VM lifetime (state creation, load,
//     run, error screen, lua_close and its __gc handlers)
// Only one runner is active at a time, so one PSP user exists at a time.
// Interrupt handlers always run on the MSP, whatever the thread uses.
//
// app_stack_run() paints the stack, arms PSPLIM above a 32-byte guard at the
// bottom (a push past it is a STKOF HardFault instead of silent corruption),
// switches Thread mode to the PSP, calls fn(arg), and switches back. The
// HardFault handler reads g_app_stack_base/owner to name the stack.
//
// Simulator: there is no PSP; fn(arg) runs directly on the host stack.
// =============================================================================

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  APP_STACK_NONE = 0,
  APP_STACK_NATIVE = 1,
  APP_STACK_LUA = 2,
} app_stack_owner_t;

// Bottom-of-stack guard kept below PSPLIM, and the paint word used both for
// the guard and for high-water measurement.
#define APP_STACK_GUARD_BYTES 32u
#define APP_STACK_PAINT 0xDEADBEEFu

// The stack installed as PSP right now; NULL / APP_STACK_NONE when no app
// runtime is on it. Read by the HardFault handler (main.c).
extern uint8_t *volatile g_app_stack_base;
extern volatile uint32_t g_app_stack_size;
extern volatile uint8_t g_app_stack_owner;

// Run fn(arg) on [base, base+size) as the Thread-mode PSP. Blocks until fn
// returns. base must stay allocated until then.
void app_stack_run(uint8_t *base, uint32_t size, app_stack_owner_t owner,
                   void (*fn)(void *), void *arg);

// True if the guard words at the bottom of the stack are untouched.
bool app_stack_guard_intact(const uint8_t *base);

// Peak bytes used since the stack was painted (scan from the bottom for the
// first word that is no longer the paint pattern). 0 in the simulator.
uint32_t app_stack_high_water(const uint8_t *base, uint32_t size);

// Core 0 MSP high-water: main() paints the free part of the 4 KB main stack
// at boot (app_stack_paint_msp); app_stack_msp_high_water() reports the peak
// bytes used since then. Both no-ops / 0 in the simulator.
void app_stack_paint_msp(void);
uint32_t app_stack_msp_high_water(void);
