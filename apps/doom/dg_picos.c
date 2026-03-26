#include "doomgeneric.h"
#include "app_abi.h"
#include "os.h"
#include "opl_capture.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>

// Doom's frame counter - declared in d_loop.c
extern int gametic;

// Doom's palette-indexed screen buffer (320x200 bytes) - declared in i_video.c
extern unsigned char *I_VideoBuffer;

// Big-endian RGB565 palette LUT - built by I_SetPalette in i_video.c
extern uint16_t rgb565_be_palette[256];

// --- Global PicOS state ---
const PicoCalcAPI *g_picos_api;
char g_app_dir[128];
static const PicoCalcAPI *s_api;
static bool s_show_fps = false;
static uint32_t s_last_gametic = 0;

// longjmp target for _exit() — lets DOOM's exit()/I_Error() return to the
// OS instead of spinning in while(1).
jmp_buf g_exit_jmp;

// Doom renders to rows 60-259 (200 rows) on 320x320 screen
#define DOOM_Y_OFFSET 60
#define DOOM_Y_END 259

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
    // Only draw and flush when Doom has produced a new frame.
    // This provides frame-skip: if rendering is slower than Doom's 35Hz tic rate,
    // we skip the unnecessary display update and let Doom catch up.
    if (gametic != s_last_gametic) {
        s_last_gametic = gametic;

        // I_VideoBuffer is NULL before I_InitGraphics allocates it, and may be
        // NULL on second launch if the zone allocator hasn't re-allocated yet.
        if (!I_VideoBuffer) {
            static bool s_logged_null = false;
            if (!s_logged_null) {
                s_api->sys->log("DOOM: I_VideoBuffer is NULL, skipping frame");
                s_logged_null = true;
            }
            return;
        }

        s_api->perf->beginFrame();

        // One-pass palette→framebuffer: convert I_VideoBuffer (palette indices)
        // directly to big-endian RGB565 in the SRAM framebuffer using the LUT.
        // This eliminates the intermediate DG_ScreenBuffer and the drawImageNN
        // byte-swap, saving ~128KB of PSRAM reads/writes per frame.
        uint16_t *fb = s_api->display->getBackBuffer();
        if (!fb) {
            s_api->perf->endFrame();
            return;
        }

        static bool s_first_frame = true;
        if (s_first_frame) {
            s_first_frame = false;
            char msg[80];
            snprintf(msg, sizeof(msg), "DOOM: fb=%p src=%p pal=%p",
                     (void*)fb, (void*)I_VideoBuffer, (void*)rgb565_be_palette);
            s_api->sys->log(msg);
        }

        const unsigned char *src = I_VideoBuffer;
        const uint16_t *pal = rgb565_be_palette;

        for (int y = 0; y < 200; y++) {
            uint16_t *dst = &fb[(y + DOOM_Y_OFFSET) * 320];
            const unsigned char *row = &src[y * 320];
            for (int x = 0; x < 320; x += 4) {
                dst[x]     = pal[row[x]];
                dst[x + 1] = pal[row[x + 1]];
                dst[x + 2] = pal[row[x + 2]];
                dst[x + 3] = pal[row[x + 3]];
            }
        }

        if (s_show_fps) {
            s_api->perf->drawFPS(250, 8);
        }

        // Flush only the active region (rows 59-260 with margin) instead of full 320x320.
        // This reduces DMA transfer by ~38% (64K pixels vs 102K pixels).
        s_api->display->flushRegion(DOOM_Y_OFFSET - 1, DOOM_Y_OFFSET + 200);

        s_api->perf->endFrame();
    }
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
        opl_capture_stop();
        api->sys->setAudioCallback(NULL);  // stop Core 1 mixing
        api->sys->log("DOOM: exit() called, returning to launcher");
        return;
    }

    // Build path to doom1.wad in app directory
    static char wad_path[256];
    snprintf(wad_path, sizeof(wad_path), "%s/doom1.wad", app_dir);

    // Pass -gfxmode rgb565 and -iwad pointing to the app directory
    char* argv[] = {"doom", "-gfxmode", "rgb565", "-iwad", wad_path, NULL};

    // Initialize DOOM (runs one tick internally, then returns)
    doomgeneric_Create(5, argv);

    // Main game loop — doomgeneric expects the platform to drive ticks
    while (!api->sys->shouldExit()) {
        api->sys->poll();

        // Toggle FPS with F3
        if (api->input->getButtonsPressed() & BTN_F3) {
            s_show_fps = !s_show_fps;
        }

        doomgeneric_Tick();
    }

    opl_capture_stop();
    api->sys->setAudioCallback(NULL);  // stop Core 1 mixing
    api->sys->log("DOOM: Exiting...");
}
