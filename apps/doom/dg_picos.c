#include "doomgeneric.h"
#include "app_abi.h"
#include "os.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>

// --- Global PicOS state ---
const PicoCalcAPI *g_picos_api;
char g_app_dir[128];
static const PicoCalcAPI *s_api;

// longjmp target for _exit() — lets DOOM's exit()/I_Error() return to the
// OS instead of spinning in while(1).
jmp_buf g_exit_jmp;

// --- Keys mapping ---
typedef struct {
    uint32_t picos_btn;
    unsigned char doom_key;
} key_map_t;

// doomgeneric uses its own key codes in doomkeys.h
#include "doomkeys.h"

static const key_map_t s_key_map[] = {
    {BTN_UP,    KEY_UPARROW},
    {BTN_DOWN,  KEY_DOWNARROW},
    {BTN_LEFT,  KEY_LEFTARROW},
    {BTN_RIGHT, KEY_RIGHTARROW},
    {BTN_ENTER, KEY_ENTER},
    {BTN_ESC,   KEY_ESCAPE},
    {BTN_CTRL,  KEY_RCTRL},
    {BTN_SHIFT, KEY_RSHIFT},
    {BTN_TAB,   KEY_TAB},
    {BTN_F4,    KEY_FIRE},
    {BTN_F5,    KEY_USE},
    {BTN_F1,    '1'},
    {BTN_F2,    '2'},
    {BTN_F3,    '3'},
    {0, 0}
};

// Map ASCII characters to DOOM key codes where needed.
static unsigned char ascii_to_doom_key(char c) {
    switch (c) {
        case ' ':  return KEY_USE;
        default:   return (unsigned char)c;
    }
}

// --- doomgeneric implementation ---

void DG_Init() {
    // Clear both framebuffers so no stale launcher content shows through.
    // The display is double-buffered — each flush() swaps buffers, so we
    // need two clear+flush cycles to ensure both are black.
    s_api->display->clear(0x0000);
    s_api->display->flush();
    s_api->display->clear(0x0000);
    s_api->display->flush();
}

void DG_DrawFrame() {
    // DG_ScreenBuffer is already RGB565 (native endian) thanks to gfxmode rgb565
    // PicOS drawImageNN expects native-endian pixels and handles the conversion
    // to big-endian for the LCD.

    // Draw to center of 320x320 screen (y=60)
    // Scale 1 (320x200 image)
    s_api->display->drawImageNN(0, 60, (const uint16_t*)DG_ScreenBuffer, 320, 200, 1);
    s_api->display->flush();
}

void DG_SleepMs(uint32_t ms) {
    // No-op for now
}

uint32_t DG_GetTicksMs() {
    return s_api->sys->getTimeMs();
}

int DG_GetKey(int* pressed, unsigned char* key) {
    static uint32_t last_buttons = 0;
    uint32_t current_buttons = s_api->input->getButtons();
    uint32_t changed = current_buttons ^ last_buttons;

    if (changed) {
        for (int i = 0; s_key_map[i].picos_btn != 0; i++) {
            if (changed & s_key_map[i].picos_btn) {
                *pressed = (current_buttons & s_key_map[i].picos_btn) ? 1 : 0;
                *key = s_key_map[i].doom_key;
                last_buttons ^= s_key_map[i].picos_btn;
                return 1;
            }
        }
    }

    // Character keys: getChar() returns the last character from kbd_poll().
    // It does NOT consume the value, so we must track the previous character
    // to avoid reporting the same key every time DG_GetKey is called within
    // a single tick (which would cause an infinite loop in DOOM's input
    // polling).  We generate a press when a new character appears and a
    // release when it disappears (next poll cycle clears it to 0).
    static char last_char = 0;
    char c = s_api->input->getChar();
    if (c && c != last_char) {
        last_char = c;
        *pressed = 1;
        *key = ascii_to_doom_key(c);
        return 1;
    } else if (!c && last_char) {
        *pressed = 0;
        *key = ascii_to_doom_key(last_char);
        last_char = 0;
        return 1;
    }

    return 0;
}

void DG_SetWindowTitle(const char * title) {
    // No window title in PicOS
}

// --- PicOS Entry Point ---

void picos_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    s_api = api;
    g_picos_api = api;
    strncpy(g_app_dir, app_dir, sizeof(g_app_dir) - 1);
    g_app_dir[sizeof(g_app_dir) - 1] = '\0';

    api->sys->log("DOOM: Starting...");

    // If DOOM calls exit() (e.g. from I_Error), longjmp back here instead
    // of spinning forever in _exit()'s while(1).
    int exit_code = setjmp(g_exit_jmp);
    if (exit_code != 0) {
        api->sys->setAudioCallback(NULL);  // stop Core 1 mixing
        api->sys->log("DOOM: exit() called, returning to launcher");
        return;
    }

    // Pass -gfxmode rgb565 to i_video.c
    char* argv[] = {"doom", "-gfxmode", "rgb565", NULL};

    // Initialize DOOM (runs one tick internally, then returns)
    doomgeneric_Create(3, argv);

    // Main game loop — doomgeneric expects the platform to drive ticks
    while (!api->sys->shouldExit()) {
        api->sys->poll();
        doomgeneric_Tick();
    }

    api->sys->setAudioCallback(NULL);  // stop Core 1 mixing
    api->sys->log("DOOM: Exiting...");
}
