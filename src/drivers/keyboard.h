#pragma once

#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// PicoCalc Keyboard Driver
// Reads key events from the STM32F103 keyboard controller via I2C1.
// The STM32 also manages battery status and LCD backlight.
//
// Key event format: 2 bytes read from REG_FIF (0x09)
//   byte[0] = state:   1=pressed, 2=hold, 3=released, 0=idle
//   byte[1] = keycode: ASCII for printable keys, or a special constant below
//
// Source: clockworkpi/PicoCalc picocalc_keyboard firmware (reg.h / keyboard.h)
// =============================================================================

// Special key codes (non-ASCII, from STM32 keyboard firmware keyboard.h)
#define KEY_UP     0xB5
#define KEY_DOWN   0xB6
#define KEY_LEFT   0xB4
#define KEY_RIGHT  0xB7
#define KEY_ENTER  0x0A   // LF — what the firmware sends for Enter
#define KEY_ESC    0xB1
#define KEY_BKSPC  0x08   // ASCII backspace
#define KEY_TAB    0x09   // ASCII tab
#define KEY_MENU   0xA4   // Sym/Fn key — used as system menu trigger (no physical F-keys on PicoCalc)
#define KEY_NONE   0x00   // No key / idle

// Modifier key codes (sent as separate events when CFG_REPORT_MODS is set)
#define KEY_MOD_ALT  0xA1
#define KEY_MOD_SHL  0xA2   // Left Shift
#define KEY_MOD_SHR  0xA3   // Right Shift
#define KEY_MOD_SYM  0xA4   // Symbol / Fn
#define KEY_MOD_CTRL 0xA5

// Init I2C1 and keyboard polling
void kbd_init(void);

// Poll the keyboard controller. Must be called once per frame.
// Populates the internal key state used by all other functions.
void kbd_poll(void);

// Returns last ASCII char typed this frame (0 = none).
// Also returns KEY_BKSPC (0x08) when backspace is pressed.
char kbd_get_char(void);

// Returns the raw keycode of the last key pressed this frame (0 = none).
// Unlike kbd_get_char(), this captures every key including arrows, F-keys,
// modifiers, and any other keycode the STM32 sends — useful for debugging
// and for mapping keys that don't have BTN_* entries yet.
uint8_t kbd_get_raw_key(void);

// Bitmask of currently held button/key states (BTN_* flags from os.h)
uint32_t kbd_get_buttons(void);

// Edge-detect: buttons that became pressed this frame
uint32_t kbd_get_buttons_pressed(void);

// Edge-detect: buttons that were released this frame
uint32_t kbd_get_buttons_released(void);

// Read battery percent from STM32 (0-100). Returns -1 on I2C error.
// Bit 7 of the raw value is a charging flag — this function masks it off.
int kbd_get_battery_percent(void);

// Set LCD backlight brightness 0-255 via STM32
void kbd_set_backlight(uint8_t brightness);
