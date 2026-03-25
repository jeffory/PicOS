// HAL Input - SDL2 Implementation

#include "hal_input.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>

static uint32_t g_buttons = 0;
static uint32_t g_buttons_pressed = 0;
static char g_char_buffer[256];
static int g_char_head = 0;
static int g_char_tail = 0;
static pthread_mutex_t s_input_mutex = PTHREAD_MUTEX_INITIALIZER;

// Key mapping table
static struct {
    SDL_Keycode sdl_key;
    uint32_t btn_mask;
} key_mappings[] = {
    {SDLK_UP, BTN_UP},
    {SDLK_DOWN, BTN_DOWN},
    {SDLK_LEFT, BTN_LEFT},
    {SDLK_RIGHT, BTN_RIGHT},
    {SDLK_RETURN, BTN_ENTER},
    {SDLK_ESCAPE, BTN_ESC},
    {SDLK_F1, BTN_F1},
    {SDLK_F2, BTN_F2},
    {SDLK_F3, BTN_F3},
    {SDLK_F4, BTN_F4},
    {SDLK_TAB, BTN_TAB},
    {SDLK_BACKSPACE, BTN_BACKSPACE},
    {SDLK_DELETE, BTN_DEL},
    {SDLK_HOME, BTN_FN},
    {SDLK_LCTRL, BTN_CTRL},
    {SDLK_RCTRL, BTN_CTRL},
    {SDLK_LSHIFT, BTN_SHIFT},
    {SDLK_RSHIFT, BTN_SHIFT},
    {SDLK_F12, BTN_F9},  // Screenshot key
    {SDLK_F10, BTN_MENU},  // System menu
    {0, 0}
};

bool hal_input_init(void) {
    memset(g_char_buffer, 0, sizeof(g_char_buffer));
    g_char_head = 0;
    g_char_tail = 0;
    pthread_mutex_init(&s_input_mutex, NULL);
    printf("[Input] Initialized\n");
    return true;
}

void hal_input_shutdown(void) {
    pthread_mutex_destroy(&s_input_mutex);
    printf("[Input] Shutdown\n");
}

void hal_input_handle_event(const SDL_Event* event) {
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        bool pressed = (event->type == SDL_KEYDOWN);
        SDL_Keycode key = event->key.keysym.sym;
        
        pthread_mutex_lock(&s_input_mutex);
        
        // Check button mappings
        for (int i = 0; key_mappings[i].sdl_key != 0; i++) {
            if (key_mappings[i].sdl_key == key) {
                if (pressed) {
                    g_buttons |= key_mappings[i].btn_mask;
                    g_buttons_pressed |= key_mappings[i].btn_mask;
                } else {
                    g_buttons &= ~key_mappings[i].btn_mask;
                }
                pthread_mutex_unlock(&s_input_mutex);
                return;
            }
        }
        
        // Character input
        if (pressed && (key >= 32 && key < 127)) {
            int next = (g_char_head + 1) % sizeof(g_char_buffer);
            if (next != g_char_tail) {
                g_char_buffer[g_char_head] = (char)key;
                g_char_head = next;
            }
        }
        
        pthread_mutex_unlock(&s_input_mutex);
    }
}

void hal_input_update(void) {
    pthread_mutex_lock(&s_input_mutex);
    // Clear pressed flags each frame (they're edge-triggered)
    g_buttons_pressed = 0;
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_inject_buttons(uint32_t buttons) {
    pthread_mutex_lock(&s_input_mutex);
    g_buttons |= buttons;
    g_buttons_pressed |= buttons;
    pthread_mutex_unlock(&s_input_mutex);
}

uint32_t hal_input_get_buttons(void) {
    pthread_mutex_lock(&s_input_mutex);
    uint32_t result = g_buttons;
    pthread_mutex_unlock(&s_input_mutex);
    return result;
}

uint32_t hal_input_get_buttons_pressed(void) {
    pthread_mutex_lock(&s_input_mutex);
    uint32_t result = g_buttons_pressed;
    pthread_mutex_unlock(&s_input_mutex);
    return result;
}

char hal_input_get_char(void) {
    pthread_mutex_lock(&s_input_mutex);
    if (g_char_head == g_char_tail) {
        pthread_mutex_unlock(&s_input_mutex);
        return 0;
    }
    char c = g_char_buffer[g_char_tail];
    g_char_tail = (g_char_tail + 1) % sizeof(g_char_buffer);
    pthread_mutex_unlock(&s_input_mutex);
    return c;
}

bool hal_input_poll_char(char* out_char) {
    pthread_mutex_lock(&s_input_mutex);
    if (g_char_head == g_char_tail) {
        pthread_mutex_unlock(&s_input_mutex);
        return false;
    }
    if (out_char) {
        *out_char = g_char_buffer[g_char_tail];
    }
    g_char_tail = (g_char_tail + 1) % sizeof(g_char_buffer);
    pthread_mutex_unlock(&s_input_mutex);
    return true;
}
