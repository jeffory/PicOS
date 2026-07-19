// QMI (CS1) PSRAM initialisation for the Pimoroni Pico Plus 2 W's onboard
// APS6404L.  Switches the chip into quad (QPI) mode with 0xEB fast reads and
// 0x38 quad writes — roughly 6x the bandwidth of the reset-default 1-bit
// serial 0x03 reads the system previously ran on.
//
// Derived from SparkFun's sfe_psram.c (MIT, (c) 2024 SparkFun Electronics),
// itself based on the CircuitPython RP2350 PSRAM bring-up.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Detect and configure the QMI CS1 PSRAM in quad mode.  Runs a write/readback
// self-test through the uncached alias; on failure it steps RXDELAY up, and
// if no setting passes it returns the chip to serial SPI mode and restores
// the reset-default M1 registers (slow but safe).
// Returns the detected PSRAM size in bytes, 0 if no PSRAM responded.
size_t qmi_psram_init(uint32_t cs_pin);

// Recompute M1 timing (CLKDIV / MAX_SELECT / MIN_DESELECT) for the current
// clk_sys.  Call after every set_sys_clock_khz change.  Safe no-op when the
// quad-mode init failed or never ran.
void qmi_psram_update_timing(void);

// True when the PSRAM is running in quad mode (init + self-test succeeded).
bool qmi_psram_is_quad(void);
