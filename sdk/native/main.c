// Hello C — minimal PicOS native app demo
//
// Build:   make
// Deploy:  copy main.elf + app.json to /apps/hello_c/ on the SD card

#include "app_abi.h"
#include "os.h"

// Convenience RGB565 macro (same as the OS uses)
#define RGB565(r, g, b) \
    ((uint16_t)(((r) & 0x1F) << 11 | ((g) & 0x3F) << 5 | ((b) & 0x1F)))

void picos_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    (void)app_dir; (void)app_id;

    const picocalc_display_t *d = api->display;
    const picocalc_sys_t     *s = api->sys;

    // ── Precompute layout ─────────────────────────────────────────────────────
    int cx = d->getWidth() / 2;
    int cy = d->getHeight() / 2;

    // Build "Running: <name>" without sprintf
    char line2[64];
    const char *prefix = "Running: ";
    char *p = line2;
    while (*prefix) *p++ = *prefix++;
    const char *n = app_name;
    while (*n) *p++ = *n++;
    *p = '\0';

    // ── Draw + flush in a loop ────────────────────────────────────────────────
    // display_flush() is non-blocking: DMA starts and the function returns
    // before the LCD has received the frame (~65 ms for 320x320 at 25 MHz).
    // Core 1 WiFi polling races those SPI pins, so a single flush can be
    // corrupted.  Redrawing every iteration (same pattern as Lua apps) keeps
    // the display stable — any bad frame is overwritten on the very next pass.
    //
    // getButtonsPressed() covers BTN_* keys (arrows, Enter, Esc, Fn-keys).
    // getChar() covers alphanumeric and punctuation keys.
    // Checking both means *any* key exits.
    while (true) {
        d->clear(0x0000);
        d->drawText(cx - 48, cy - 16, "Hello from C!", 0xFFFF, 0x0000);
        d->drawText(cx - 60, cy,       line2,           RGB565(12, 24, 20), 0x0000);
        d->drawText(cx - 52, cy + 16,  "Press any key...", RGB565(16, 32, 16), 0x0000);
        d->flush();

        s->poll();
        if (api->input->getButtonsPressed() || api->input->getChar())
            break;
    }

    // Return to launcher
}
