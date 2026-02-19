#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"

#include <stdio.h>
#include <stdarg.h>

#include "os/os.h"
#include "os/launcher.h"
#include "hardware.h"
#include "drivers/display.h"
#include "drivers/keyboard.h"
#include "drivers/sdcard.h"
#include "splash_logo.h"

// ── OS API implementation stubs (wiring the function pointer table) ───────────
// Full implementations live in each driver. This wires them all together
// into the global g_api struct that Lua and future C apps can reference.

PicoCalcAPI g_api;

static picocalc_input_t s_input_impl = {
    .getButtons         = kbd_get_buttons,
    .getButtonsPressed  = kbd_get_buttons_pressed,
    .getButtonsReleased = kbd_get_buttons_released,
    .getChar            = kbd_get_char,
};

static int display_get_width_fn(void)  { return FB_WIDTH; }
static int display_get_height_fn(void) { return FB_HEIGHT; }

static picocalc_display_t s_display_impl = {
    .clear         = display_clear,
    .setPixel      = display_set_pixel,
    .fillRect      = display_fill_rect,
    .drawRect      = display_draw_rect,
    .drawLine      = display_draw_line,
    .drawText      = display_draw_text,
    .flush         = display_flush,
    .getWidth      = display_get_width_fn,
    .getHeight     = display_get_height_fn,
    .setBrightness = display_set_brightness,
};

static uint32_t sys_getTimeMs(void) {
    return to_ms_since_boot(get_absolute_time());
}
static void sys_reboot(void) {
    watchdog_enable(1, true);
    for (;;) tight_loop_contents();
}
static bool sys_isUSBPowered(void) {
    // RP2350: check VBUS via USB power detect (GP24 on standard Pico layout)
    // Pimoroni may differ — implement if needed
    return false;
}
static void sys_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static picocalc_sys_t s_sys_impl = {
    .getTimeMs        = sys_getTimeMs,
    .reboot           = sys_reboot,
    .getBatteryPercent = kbd_get_battery_percent,
    .isUSBPowered     = sys_isUSBPowered,
    .addMenuItem      = NULL,   // TODO: system_menu.c
    .clearMenuItems   = NULL,
    .log              = sys_log,
};

// ── Boot splash ───────────────────────────────────────────────────────────────

static void draw_splash(const char *status) {
    display_clear(COLOR_BLACK);

#if LOGO_W > 0 && LOGO_H > 0
    // ── Logo ──────────────────────────────────────────────────────────────────
    int lx = (FB_WIDTH  - LOGO_W) / 2;
    int ly = (FB_HEIGHT - LOGO_H) / 2 - 16;
    display_draw_image(lx, ly, LOGO_W, LOGO_H, logo_data);

    // ── Status text below logo ─────────────────────────────────────────────
    int sx = (FB_WIDTH - display_text_width(status)) / 2;
    display_draw_text(sx, ly + LOGO_H + 12, status, COLOR_GRAY, COLOR_BLACK);
#else
    // ── No logo: centred title + status ───────────────────────────────────────
    const char *title = "PicOS";
    int tx = (FB_WIDTH - display_text_width(title)) / 2;
    display_draw_text(tx, FB_HEIGHT / 2 - 8, title, COLOR_WHITE, COLOR_BLACK);

    int sx = (FB_WIDTH - display_text_width(status)) / 2;
    display_draw_text(sx, FB_HEIGHT / 2 + 8, status, COLOR_GRAY, COLOR_BLACK);
#endif

    display_flush();
}

// ── Core 1 entry — periodic tasks (future: audio mixing, WiFi polling) ────────

static void core1_entry(void) {
    // Currently unused. Reserve Core 1 for future background tasks.
    // Do NOT touch the LCD or SPI from here without acquiring the mutex first.
    while (true) {
        tight_loop_contents();
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(void) {
    // Overclock to 200 MHz for better display throughput (RP2350 supports 150+)
    // NOTE: If the keyboard fails to initialise reliably, try commenting this
    // out to test at the default 125 MHz — it isolates whether the overclock
    // is affecting I2C timing.
    set_sys_clock_khz(200000, true);
    
    // Configure peripheral clock to 125 MHz (enables 62.5 MHz SPI for LCD)
    // clk_peri drives UART, SPI, I2C, PWM — ST7789 rated max is 62.5 MHz
    clock_configure(clk_peri,
        0,                               // No glitchless mux
        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,  // Source: PLL_SYS (200MHz)
        200 * MHZ,                       // Input frequency
        125 * MHZ);                      // Output: 125 MHz → SPI can reach 62.5 MHz
    
    stdio_init_all();

    // Wait up to 3 s for a USB serial host to connect so early printf output
    // isn't lost. Skips automatically if already connected.
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);

    printf("\n--- PicoCalc OS booting ---\n");

    // Wire up the global API struct
    g_api.input   = &s_input_impl;
    g_api.display = &s_display_impl;
    g_api.sys     = &s_sys_impl;
    // fs and audio wired after their init below

    // Initialise display first so we can show progress
    display_init();
    draw_splash("Initialising keyboard...");

    kbd_init();
    kbd_set_backlight(128);

    draw_splash("Mounting SD card...");
    bool sd_ok = sdcard_init();

    if (!sd_ok) {
        display_clear(COLOR_BLACK);
        display_draw_text(8, 8,  "SD card not found!", COLOR_RED,  COLOR_BLACK);
        display_draw_text(8, 20, "Insert a FAT32 SD card", COLOR_WHITE, COLOR_BLACK);
        display_draw_text(8, 32, "and press A to retry.", COLOR_GRAY, COLOR_BLACK);
        display_flush();

        // Wait for A press then try again
        while (true) {
            kbd_poll();
            if (kbd_get_buttons_pressed() & BTN_ENTER) {
                sd_ok = sdcard_remount();
                if (sd_ok) break;
            }
            sleep_ms(100);
        }
    }

    printf("SD card mounted OK\n");

    // Launch Core 1 background tasks
    multicore_launch_core1(core1_entry);

    draw_splash("Loading...");
    sleep_ms(300);   // Brief pause so the splash is visible

    // Hand off to the launcher — this never returns
    launcher_run();

    // Unreachable
    return 0;
}
