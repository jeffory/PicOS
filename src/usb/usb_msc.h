#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Blockingly enter MSC USB mode until ESC is pressed on the Pico keyboard
// Unmounts SD Card, turns on MSC support, loops, then remounts.
void usb_msc_enter_mode(void);

#ifdef __cplusplus
}
#endif
