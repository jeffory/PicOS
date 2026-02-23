#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/xip.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>

#include "drivers/audio.h"
#include "drivers/sound.h"
#include "drivers/display.h"
#include "drivers/http.h"
#include "drivers/keyboard.h"
#include "drivers/sdcard.h"
#include "drivers/wifi.h"
#include "hardware.h"
#include "os/config.h"
#include "os/launcher.h"
#include "os/lua_psram_alloc.h"
#include "os/os.h"
#include "os/system_menu.h"
#include "os/ui.h"

// ── OS API implementation stubs (wiring the function pointer table)
// ─────────── Full implementations live in each driver. This wires them all
// together into the global g_api struct that Lua and future C apps can
// reference.

PicoCalcAPI g_api;

static picocalc_input_t s_input_impl = {
    .getButtons = kbd_get_buttons,
    .getButtonsPressed = kbd_get_buttons_pressed,
    .getButtonsReleased = kbd_get_buttons_released,
    .getChar = kbd_get_char,
};

static int display_get_width_fn(void) { return FB_WIDTH; }
static int display_get_height_fn(void) { return FB_HEIGHT; }

static picocalc_display_t s_display_impl = {
    .clear = display_clear,
    .setPixel = display_set_pixel,
    .fillRect = display_fill_rect,
    .drawRect = display_draw_rect,
    .drawLine = display_draw_line,
    .drawText = display_draw_text,
    .flush = display_flush,
    .getWidth = display_get_width_fn,
    .getHeight = display_get_height_fn,
    .setBrightness = display_set_brightness,
};

static uint32_t sys_getTimeMs(void) {
  return to_ms_since_boot(get_absolute_time());
}
static void sys_reboot(void) {
  watchdog_enable(1, true);
  for (;;)
    tight_loop_contents();
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
    .getTimeMs = sys_getTimeMs,
    .reboot = sys_reboot,
    .getBatteryPercent = kbd_get_battery_percent,
    .isUSBPowered = sys_isUSBPowered,
    .addMenuItem = system_menu_add_item,
    .clearMenuItems = system_menu_clear_items,
    .log = sys_log,
};

static picocalc_wifi_t s_wifi_impl = {
    .connect = wifi_connect,
    .disconnect = wifi_disconnect,
    .getStatus = wifi_get_status,
    .getIP = wifi_get_ip,
    .getSSID = wifi_get_ssid,
    .isAvailable = wifi_is_available,
};

static picocalc_audio_t s_audio_impl = {
    .playTone = audio_play_tone,
    .stopTone = audio_stop_tone,
    .setVolume = audio_set_volume,
};

// ── Core 1 entry — periodic tasks (future: audio mixing, WiFi polling)
// ────────

static void core1_entry(void) {
  // Currently unused. Reserve Core 1 for future background tasks.
  // Do NOT touch the LCD or SPI from here without acquiring the mutex first.
  // Use __wfe() (Wait For Event) for lower power than tight_loop_contents().
  while (true) {
    __wfe();
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
  clock_configure(
      clk_peri,
      0,                                                // No glitchless mux
      CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, // Source: PLL_SYS
                                                        // (200MHz)
      200 * MHZ,                                        // Input frequency
      200 * MHZ); // Output: 200 MHz → SPI can reach 100 MHz

  stdio_init_all();

  // Wait up to 3 s for a USB serial host to connect so early printf output
  // isn't lost. Skips automatically if already connected.
  for (int i = 0; i < 30 && !stdio_usb_connected(); i++)
    sleep_ms(100);

  printf("\n--- PicoCalc OS booting ---\n");

  // Wire up the global API struct
  g_api.input = &s_input_impl;
  g_api.display = &s_display_impl;
  g_api.sys = &s_sys_impl;
  g_api.wifi = &s_wifi_impl;
  g_api.audio = &s_audio_impl;
  // fs wired after SD card init

  // Explicitly configure PSRAM hardware pins and XIP write logic for the Pico
  // Plus 2W before any PSRAM pointers are accessed.
#ifdef PICO_RP2350
  gpio_set_function(47, GPIO_FUNC_XIP_CS1);
  xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;
#endif

  // Initialise display first so we can show progress
  display_init();
  sound_init();
  ui_draw_splash("Initialising keyboard...", NULL);

  bool kbd_ok = kbd_init();
  if (kbd_ok) {
    kbd_set_backlight(128);
  } else {
    // Keyboard failed - STM32 didn't respond
    display_clear(COLOR_BLACK);
    display_draw_text(8, 8, "Keyboard Controller Error!", COLOR_RED,
                      COLOR_BLACK);
    display_draw_text(8, 20, "STM32 (I2C 0x%02X) NACK.", COLOR_WHITE,
                      COLOR_BLACK);
    display_draw_text(8, 36, "The bus may be stuck.", COLOR_GRAY, COLOR_BLACK);
    display_draw_text(8, 48, "Try power cycling device.", COLOR_GRAY,
                      COLOR_BLACK);
    display_flush();
    // We can't wait for a keypress if the keyboard is dead,
    // but we'll wait a few seconds so the user can see the error.
    sleep_ms(5000);
  }

  ui_draw_splash("Mounting SD card...", NULL);
  bool sd_ok = sdcard_init();

  if (!sd_ok) {
    display_clear(COLOR_BLACK);
    display_draw_text(8, 8, "SD card not found!", COLOR_RED, COLOR_BLACK);
    display_draw_text(8, 20, "Insert a FAT32 SD card", COLOR_WHITE,
                      COLOR_BLACK);
    display_draw_text(8, 32, "and press Enter to retry.", COLOR_GRAY,
                      COLOR_BLACK);
    display_flush();

    // Wait for A press then try again
    while (true) {
      kbd_poll();
      if (kbd_get_buttons_pressed() & BTN_ENTER) {
        sd_ok = sdcard_remount();
        if (sd_ok)
          break;
      }
      sleep_ms(100);
    }
  }

  printf("SD card mounted OK\n");

  // Initialize the PSRAM allocator BEFORE anything that uses it
  // (config_load, WiFi, Lua, etc.)
  lua_psram_alloc_init();

  // Load persisted settings from /system/config.json
  config_load();

  // Initialise WiFi hardware (auto-connects if credentials are in config)
  ui_draw_splash("Initialising WiFi...", NULL);
  wifi_init();
  http_init();
  
  // Debug: check free size after WiFi/HTTP init
  extern size_t lua_psram_alloc_free_size(void);
  printf("[PSRAM] Free after WiFi/HTTP init: %zu bytes (%zuK)\n",
         lua_psram_alloc_free_size(), lua_psram_alloc_free_size() / 1024);

  // Launch Core 1 background tasks
  multicore_launch_core1(core1_entry);

  system_menu_init();

  ui_draw_splash("Loading...", NULL);
  sleep_ms(300); // Brief pause so the splash is visible

  // Hand off to the launcher — this never returns
  launcher_run();

  // Unreachable
  return 0;
}
