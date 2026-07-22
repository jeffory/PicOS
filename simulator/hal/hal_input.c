// HAL Input - SDL2 Implementation

#include "hal_input.h"
#include "hal_timing.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>

// Minimum wall-clock duration an injected one-shot ("click") press stays
// asserted before hal_input_read_buttons()/hal_input_update() auto-release
// it. This mirrors src/drivers/keyboard.c's KBD_INJECT_HOLD_MS, and for the
// exact same reason: simulator/CMakeLists.txt links THIS file (plus
// stubs/keyboard_stub.c) instead of src/drivers/keyboard.c, so the simulator
// has its own, separate injected-button state machine with the same
// "auto-release on the very next read" bug — an extra read landing between
// injection and the app's actual button sample (e.g. a watchdog-feed pump
// calling kbd_poll() more than once per frame) can retire the press before
// anything ever observes it. See keyboard.c's KBD_INJECT_HOLD_MS comment for
// the full history; this is the same fix applied to the code path every e2e
// test actually exercises.
//
// hal_get_time_ms() (SDL_GetTicks) is real wall-clock time — it is NOT
// scaled by hal_set_time_multiplier(), which only stretches/shrinks
// hal_sleep_ms()/hal_sleep_us() delays — so this hold behaves identically
// regardless of sim speed, just like the device's to_ms_since_boot().
#define HAL_INJECT_HOLD_MS 80

static uint32_t g_buttons = 0;
static uint32_t g_buttons_pressed = 0;
static uint32_t g_injected_click = 0;    // one-shot injection(s), held >= HAL_INJECT_HOLD_MS before auto-release
static uint32_t g_injected_click_since_ms = 0; // wall time g_injected_click was last (re)published
static uint32_t g_injected_click_pending = 0;  // same-button re-injection queued while still active
static uint32_t g_injected_latched = 0;  // held injections, released only by hal_input_release_buttons
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

// Retire g_injected_click once it has been held for at least
// HAL_INJECT_HOLD_MS, and publish any queued same-button re-injection.
// Must be called with s_input_mutex already held. Shared by
// hal_input_update() and hal_input_read_buttons() so the two entry points
// (one currently unused, kept for API completeness) can't drift apart.
static void retire_and_publish_injected_click_locked(uint32_t now_ms) {
    bool retired_this_read = false;
    if (g_injected_click &&
        (now_ms - g_injected_click_since_ms >= HAL_INJECT_HOLD_MS)) {
        g_buttons &= ~g_injected_click;
        g_injected_click = 0;
        retired_this_read = true;
    }
    // Publish a queued re-injection only once active is empty AND we didn't
    // just retire it in this very call — guarantees at least one full read
    // where the button reads as released before it (or a same-button
    // re-injection received while still active) can be republished. A real
    // release-then-press edge, mirroring keyboard.c's
    // injected_retired_this_poll guard.
    if (!retired_this_read && !g_injected_click && g_injected_click_pending) {
        g_buttons |= g_injected_click_pending;
        g_buttons_pressed |= g_injected_click_pending;
        g_injected_click = g_injected_click_pending;
        g_injected_click_pending = 0;
        g_injected_click_since_ms = now_ms;
    }
}

void hal_input_update(void) {
    pthread_mutex_lock(&s_input_mutex);
    // Clear pressed flags each frame (they're edge-triggered)
    g_buttons_pressed = 0;
    retire_and_publish_injected_click_locked(hal_get_time_ms());
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_read_buttons(uint32_t* out_buttons, uint32_t* out_pressed) {
    pthread_mutex_lock(&s_input_mutex);
    // Latched holds persist until an explicit hal_input_release_buttons —
    // enables modifier chords. One-shot (click) injections retire on their
    // own wall-clock hold below, not on every read.
    retire_and_publish_injected_click_locked(hal_get_time_ms());
    if (out_buttons) *out_buttons = g_buttons;
    if (out_pressed) *out_pressed = g_buttons_pressed;
    g_buttons_pressed = 0;
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_inject_buttons(uint32_t buttons) {
    pthread_mutex_lock(&s_input_mutex);
    uint32_t already_active = buttons & g_injected_click;
    uint32_t fresh = buttons & ~already_active;
    if (fresh) {
        g_buttons |= fresh;
        g_buttons_pressed |= fresh;
        g_injected_click |= fresh;
        g_injected_click_since_ms = hal_get_time_ms();
    }
    if (already_active) {
        // Same button injected again while its previous click is still
        // active — queue it instead of re-OR-ing an already-set bit (which
        // would produce no observable release/press edge at all). See
        // retire_and_publish_injected_click_locked() for the republish
        // rule. NOTE: like keyboard.c's s_injected_pending, this is a single
        // bitmask, not a per-button queue — two DIFFERENT buttons injected
        // within the same hold window merge into a momentary chord. The MCP
        // `keypress` tool's default 100ms inter-key delay is comfortably
        // above HAL_INJECT_HOLD_MS (80ms), so back-to-back sequence presses
        // never actually overlap in practice.
        g_injected_click_pending |= already_active;
    }
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_hold_buttons(uint32_t buttons) {
    pthread_mutex_lock(&s_input_mutex);
    g_buttons |= buttons;
    g_buttons_pressed |= buttons;
    g_injected_latched |= buttons;
    // A held button must not be auto-released by an earlier click of the
    // same key, nor resurrected later by a queued re-injection of it.
    g_injected_click &= ~buttons;
    g_injected_click_pending &= ~buttons;
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_release_buttons(uint32_t buttons) {
    pthread_mutex_lock(&s_input_mutex);
    g_buttons &= ~buttons;
    g_injected_click &= ~buttons;
    g_injected_click_pending &= ~buttons;
    g_injected_latched &= ~buttons;
    pthread_mutex_unlock(&s_input_mutex);
}

void hal_input_inject_char(char c) {
    pthread_mutex_lock(&s_input_mutex);
    int next = (g_char_head + 1) % sizeof(g_char_buffer);
    if (next != g_char_tail) {
        g_char_buffer[g_char_head] = c;
        g_char_head = next;
    }
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
