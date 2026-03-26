#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Blockingly enter MSC USB mode until ESC is pressed on the Pico keyboard
// Unmounts SD Card, turns on MSC support, loops, then remounts.
void usb_msc_enter_mode(void);

// Returns true when USB MSC mode is active (disk I/O runs in IRQ context —
// callers must NOT use printf, which deadlocks on CDC serial in USBCTRL_IRQ).
bool usb_msc_is_active(void);

#ifdef __cplusplus
}
#endif
