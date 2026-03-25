// Keyboard driver stub for simulator
// Bridges SDL2 input to PicOS keyboard driver interface

#include "../../src/drivers/keyboard.h"
#include "../hal/hal_input.h"
#include "../../src/os/os.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>

// Global exit flag
static volatile int *g_simulator_running = NULL;

void set_simulator_exit_flag(volatile int *flag) {
    g_simulator_running = flag;
}

void request_simulator_exit(void) {
    // Set the global g_running flag so the Lua debug hook sees it
    extern volatile int g_running;
    g_running = 0;
    if (g_simulator_running) {
        *g_simulator_running = 0;
    }
}

static uint32_t s_buttons = 0;
static uint32_t s_buttons_pressed = 0;
static uint32_t s_buttons_released = 0;
static char s_last_char = 0;
static uint8_t s_raw_key = 0;
static bool s_menu_pressed = false;
static bool s_screenshot_pressed = false;

// Key mapping from SDL to keyboard keycodes
static struct {
    uint32_t btn_mask;
    uint8_t keycode;
} btn_to_keycode[] = {
    {BTN_UP, KEY_UP},
    {BTN_DOWN, KEY_DOWN},
    {BTN_LEFT, KEY_LEFT},
    {BTN_RIGHT, KEY_RIGHT},
    {BTN_ENTER, KEY_ENTER},
    {BTN_ESC, KEY_ESC},
    {BTN_F1, KEY_F1},
    {BTN_F2, KEY_F2},
    {BTN_F3, KEY_F3},
    {BTN_F4, KEY_F4},
    {BTN_TAB, KEY_TAB},
    {BTN_BACKSPACE, KEY_BKSPC},
    {BTN_FN, KEY_MOD_SYM},
    {0, 0}
};

bool kbd_init(void) {
    printf("[KBD] Keyboard initialized (simulator)\n");
    return true;
}

void kbd_poll(void) {
    // Pull pending events from the Wayland socket into SDL's internal queue.
    // Must be called before SDL_PollEvent to avoid blocking on compositor I/O.
    SDL_PumpEvents();

    // Process SDL events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            // Handle quit - set both the running flag and dev_commands exit
            extern void request_simulator_exit(void);
            extern void dev_commands_set_exit(void);
            request_simulator_exit();
            dev_commands_set_exit();
        } else {
            hal_input_handle_event(&event);
        }
    }
    
    // Get current button state from HAL
    uint32_t new_buttons = hal_input_get_buttons();
    uint32_t new_pressed = hal_input_get_buttons_pressed();
    
    // Calculate released buttons
    s_buttons_released = s_buttons & ~new_buttons;
    
    // Update state
    s_buttons = new_buttons;
    s_buttons_pressed = new_pressed;
    
    // Check for menu key (F10 or BTN_MENU)
    if (s_buttons_pressed & BTN_MENU) {
        s_menu_pressed = true;
    }
    
    // Check for screenshot key (F12 mapped to BTN_F9)
    if (s_buttons_pressed & BTN_F9) {
        s_screenshot_pressed = true;
    }
    
    // Get character input
    s_last_char = hal_input_get_char();
    if (s_last_char) {
        s_raw_key = (uint8_t)s_last_char;
    } else if (s_buttons_pressed) {
        // Map button to keycode for raw key
        for (int i = 0; btn_to_keycode[i].btn_mask != 0; i++) {
            if (s_buttons_pressed & btn_to_keycode[i].btn_mask) {
                s_raw_key = btn_to_keycode[i].keycode;
                break;
            }
        }
    } else {
        s_raw_key = 0;
    }
    
    // Update HAL for next frame
    hal_input_update();
}

char kbd_get_char(void) {
    char c = s_last_char;
    s_last_char = 0;
    return c;
}

uint8_t kbd_get_raw_key(void) {
    uint8_t k = s_raw_key;
    s_raw_key = 0;
    return k;
}

uint32_t kbd_get_buttons(void) {
    return s_buttons;
}

uint32_t kbd_get_buttons_pressed(void) {
    return s_buttons_pressed;
}

uint32_t kbd_get_buttons_released(void) {
    return s_buttons_released;
}

int kbd_get_battery_percent(void) {
    return 100;  // Always full in simulator
}

void kbd_set_backlight(uint8_t brightness) {
    (void)brightness;
}

void kbd_apply_clock(void) {
    // No-op in simulator
}

bool kbd_consume_menu_press(void) {
    if (s_menu_pressed) {
        s_menu_pressed = false;
        return true;
    }
    return false;
}

bool kbd_consume_screenshot_press(void) {
    if (s_screenshot_pressed) {
        s_screenshot_pressed = false;
        return true;
    }
    return false;
}

void kbd_clear_state(void) {
    s_buttons = 0;
    s_buttons_pressed = 0;
    s_buttons_released = 0;
    s_last_char = 0;
    s_raw_key = 0;
}

void kbd_recover_i2c_bus(void) {
    // No-op in simulator
}

void kbd_inject_buttons(uint32_t buttons) {
    // Inject through HAL so the next kbd_poll picks them up
    hal_input_inject_buttons(buttons);
    // Also set local state for immediate reads
    s_buttons |= buttons;
    s_buttons_pressed |= buttons;
}

void kbd_inject_char(char c) {
    s_last_char = c;
    s_raw_key = (uint8_t)c;
}
