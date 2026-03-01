#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/xip.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>

// Forward declarations for display functions used in hardfault_c below.
// The full #include "drivers/display.h" appears later in the file after other
// includes; these declarations let the fault handler use them before that point.
#include <stdint.h>
void display_clear(uint16_t color);
int  display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
void display_flush(void);

// Linker symbols for the main stack limits (see boot2/memmap_*.ld)
extern uint32_t __StackTop;    // initial SP (stack grows DOWN from here)
extern uint32_t __StackBottom; // lowest valid address (4KB below StackTop)

// ── HardFault handler ─────────────────────────────────────────────────────────
// Captures the exception frame (stacked registers) and prints fault registers
// to UART+USB so we can identify the crash address.  UART stdio is polling-
// based, so this works even with interrupts disabled inside the fault handler.

static void __attribute__((used)) hardfault_c(uint32_t *frame) {
  // ARM exception frame layout (8 words pushed by hardware on entry):
  //   frame[0]=R0, [1]=R1, [2]=R2, [3]=R3,
  //   [4]=R12, [5]=LR(EXC), [6]=PC(fault), [7]=xPSR
  uint32_t pc   = frame[6];
  uint32_t lr   = frame[5];
  uint32_t r0   = frame[0];
  // Cortex-M33 fault status registers
  uint32_t cfsr = *(volatile uint32_t *)0xE000ED28u; // CFSR (UFSR|BFSR|MMFSR)
  uint32_t hfsr = *(volatile uint32_t *)0xE000ED2Cu; // HFSR
  uint32_t bfar = *(volatile uint32_t *)0xE000ED38u; // BFAR (if BFARVALID)
  uint32_t mmar = *(volatile uint32_t *)0xE000ED34u; // MMFAR (if MMFARVALID)

  // frame IS the MSP/PSP just after the hardware pushed the 8-word exception
  // frame.  Pre-fault SP = frame + 32 (8 words × 4 bytes).
  uint32_t sp_at_fault = (uint32_t)(uintptr_t)frame + 32u;

  // ── UART output (always works — polling-based, no IRQ required) ────────────
  printf("\n!!! HARDFAULT !!!\n");
  printf("  PC   = 0x%08lx\n", (unsigned long)pc);
  printf("  LR   = 0x%08lx\n", (unsigned long)lr);
  printf("  R0   = 0x%08lx\n", (unsigned long)r0);
  bool stack_overflow = (sp_at_fault < (uint32_t)(uintptr_t)&__StackBottom);
  printf("  SP   = 0x%08lx  (frame @ 0x%08lx)  stack_limit=0x%08lx%s\n",
         (unsigned long)sp_at_fault, (unsigned long)(uintptr_t)frame,
         (unsigned long)(uintptr_t)&__StackBottom,
         stack_overflow ? " *** OVERFLOW ***" : "");
  printf("  CFSR = 0x%08lx\n", (unsigned long)cfsr);
  printf("  HFSR = 0x%08lx\n", (unsigned long)hfsr);
  printf("  BFAR = 0x%08lx\n", (unsigned long)bfar);
  printf("  MMAR = 0x%08lx\n", (unsigned long)mmar);
  // Decode CFSR flags to UART for easy diagnosis
  if (cfsr & (1u<<17)) printf("  INVSTATE: invalid CPU state (bad Thumb bit?)\n");
  if (cfsr & (1u<<16)) printf("  UNDEFINSTR: undefined instruction\n");
  if (cfsr & (1u<<18)) printf("  INVPC: invalid EXC_RETURN / PC load\n");
  if (cfsr & (1u<<19)) printf("  NOCP: coprocessor access\n");
  if (cfsr & (1u<< 9)) printf("  PRECISERR: precise data bus fault (BFAR=0x%08lx)\n", (unsigned long)bfar);
  if (cfsr & (1u<< 8)) printf("  IBUSERR: instruction bus fault\n");
  if (cfsr & (1u<<10)) printf("  IMPRECISERR: imprecise data bus fault\n");
  if (cfsr & (1u<<12)) printf("  STKERR: exception stack push fault (stack overflow?)\n");
  if (cfsr & (1u<<11)) printf("  UNSTKERR: exception stack pop fault\n");
  if (cfsr & (1u<< 1)) printf("  DACCVIOL: MPU data access violation (MMAR=0x%08lx)\n", (unsigned long)mmar);
  if (cfsr & (1u<< 0)) printf("  IACCVIOL: MPU instr access violation (MMAR=0x%08lx)\n", (unsigned long)mmar);
  if (cfsr & (1u<<25)) printf("  DIVBYZERO: divide by zero\n");
  if (cfsr & (1u<<24)) printf("  UNALIGNED: unaligned access\n");
  if (hfsr & (1u<<30)) printf("  HFSR FORCED: escalated from configurable fault\n");
  if (hfsr & (1u<< 1)) printf("  HFSR VECTTBL: vector table read fault\n");
  stdio_flush();

  // ── Display output (best-effort — lets us see fault info without a UART) ──
  // display_clear/draw_text only write to the PSRAM framebuffer (no SPI).
  // display_flush sends the buffer to the LCD via PIO+DMA (blocking).
  // Uses static buffers to avoid touching an already-damaged stack.
  static char ln[56]; // static to avoid stack usage
  display_clear(0x0000); // black

  snprintf(ln, sizeof(ln), "!!! HARDFAULT !!!");
  display_draw_text(4,  4, ln, 0xF800, 0x0000); // red

  snprintf(ln, sizeof(ln), "PC %08lx  LR %08lx", (unsigned long)pc, (unsigned long)lr);
  display_draw_text(4, 20, ln, 0xFFFF, 0x0000);

  {
    bool stk_ovfl = (sp_at_fault < (uint32_t)(uintptr_t)&__StackBottom);
    snprintf(ln, sizeof(ln), "SP %08lx  lim %08lx%s",
             (unsigned long)sp_at_fault,
             (unsigned long)(uintptr_t)&__StackBottom,
             stk_ovfl ? " OVFL!" : "");
    display_draw_text(4, 36, ln, stk_ovfl ? 0xF800 : 0xFFFF, 0x0000);
  }

  snprintf(ln, sizeof(ln), "CFSR %08lx  HFSR %08lx", (unsigned long)cfsr, (unsigned long)hfsr);
  display_draw_text(4, 52, ln, 0xFFFF, 0x0000);

  snprintf(ln, sizeof(ln), "BFAR %08lx  MMAR %08lx", (unsigned long)bfar, (unsigned long)mmar);
  display_draw_text(4, 68, ln, 0xFFFF, 0x0000);

  // Decode CFSR fault type flags on-screen
  int y = 88;
  uint16_t warn = 0xFD20; // orange
  if (cfsr & (1u<<17)) { display_draw_text(4, y, "INVSTATE: invalid CPU state", warn, 0); y += 14; }
  if (cfsr & (1u<<16)) { display_draw_text(4, y, "UNDEFINSTR", warn, 0);                  y += 14; }
  if (cfsr & (1u<<18)) { display_draw_text(4, y, "INVPC: bad EXC_RETURN/PC",  warn, 0);  y += 14; }
  if (cfsr & (1u<<19)) { display_draw_text(4, y, "NOCP: coprocessor",          warn, 0); y += 14; }
  if (cfsr & (1u<< 9)) { display_draw_text(4, y, "PRECISERR: data bus fault",  warn, 0); y += 14; }
  if (cfsr & (1u<< 8)) { display_draw_text(4, y, "IBUSERR: instr bus fault",   warn, 0); y += 14; }
  if (cfsr & (1u<<10)) { display_draw_text(4, y, "IMPRECISERR: data bus",       warn, 0); y += 14; }
  if (cfsr & (1u<<12)) { display_draw_text(4, y, "STKERR: stack push fault!",   warn, 0); y += 14; }
  if (cfsr & (1u<<11)) { display_draw_text(4, y, "UNSTKERR: stack pop fault",   warn, 0); y += 14; }
  if (cfsr & (1u<< 1)) { display_draw_text(4, y, "DACCVIOL: MPU data viol",    warn, 0); y += 14; }
  if (cfsr & (1u<< 0)) { display_draw_text(4, y, "IACCVIOL: MPU instr viol",   warn, 0); y += 14; }
  if (cfsr & (1u<<25)) { display_draw_text(4, y, "DIVBYZERO",                   warn, 0); y += 14; }
  if (cfsr & (1u<<24)) { display_draw_text(4, y, "UNALIGNED access",            warn, 0); y += 14; }
  if (hfsr & (1u<<30)) { display_draw_text(4, y, "HFSR: FORCED escalation",    warn, 0); y += 14; }
  if (hfsr & (1u<< 1)) { display_draw_text(4, y, "HFSR: vector table fault",   warn, 0); y += 14; }
  if (y == 72)          { display_draw_text(4, y, "(no CFSR flags set)",        warn, 0); }

  display_flush();

  while (1) tight_loop_contents();
}

// Naked trampoline: inspect EXC_RETURN to find which stack held the frame,
// then pass its address to the C handler.
void __attribute__((naked)) isr_hardfault(void) {
  __asm volatile (
    "tst  lr, #4     \n" // bit 2 of EXC_RETURN: 0=MSP, 1=PSP
    "ite  eq         \n"
    "mrseq r0, msp   \n" // frame on MSP (normal for thread mode without RTOS)
    "mrsne r0, psp   \n" // frame on PSP (if PSP was active thread stack)
    "b    hardfault_c\n"
  );
}

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
  return gpio_get(USB_VBUS_PIN);
}
static void sys_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

// Native-app tick: poll keyboard (for input) and fire pending C HTTP callbacks.
// WiFi is handled by Core 1; call this in your main loop for input responsiveness.
static void sys_poll(void) {
  kbd_poll();
  http_fire_c_pending();
}

static picocalc_sys_t s_sys_impl = {
    .getTimeMs = sys_getTimeMs,
    .reboot = sys_reboot,
    .getBatteryPercent = kbd_get_battery_percent,
    .isUSBPowered = sys_isUSBPowered,
    .addMenuItem = system_menu_add_item,
    .clearMenuItems = system_menu_clear_items,
    .log = sys_log,
    .poll = sys_poll,
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

static pcfile_t fs_open(const char *path, const char *mode) {
    return (pcfile_t)sdcard_fopen(path, mode);
}
static int fs_read(pcfile_t f, void *buf, int len) {
    return sdcard_fread((sdfile_t)f, buf, len);
}
static int fs_write(pcfile_t f, const void *buf, int len) {
    return sdcard_fwrite((sdfile_t)f, buf, len);
}
static void fs_close(pcfile_t f) {
    sdcard_fclose((sdfile_t)f);
}
static bool fs_exists(const char *path) {
    return sdcard_fexists(path);
}
static int fs_size(const char *path) {
    return sdcard_fsize(path);
}

// Static-global callback state avoids passing a stack-allocated pointer
// through the deep FatFS call chain (f_opendir → f_readdir → ...).
// A stack pointer can be clobbered by an ISR or a compiler tail-call before
// sdcard_list_dir's callback fires, causing an INVSTATE hard fault.
static struct {
    void (*fn)(const char *, bool, void *);
    void *user;
} s_list_cb;

static void fs_list_dir_callback(const sdcard_entry_t *entry, void *user) {
    (void)user;
    printf("[FS_CB] entry=%s fn=%p user=%p\n",
           entry->name, (void*)s_list_cb.fn, s_list_cb.user);
    s_list_cb.fn(entry->name, entry->is_dir, s_list_cb.user);
}

static int fs_list_dir(const char *path,
                       void (*callback)(const char *name, bool is_dir, void *user),
                       void *user) {
    printf("[FS_LISTDIR] path=%s cb=%p user=%p\n",
           path, (void*)callback, user);

    // Diagnostic: check stack headroom before deep FatFS call
    extern uint32_t __StackBottom;
    uint32_t sp = (uint32_t)__get_MSP();
    uint32_t headroom = sp - (uint32_t)&__StackBottom;
    printf("[FS] stack sp=%08lx bottom=%08lx headroom=%lu\n", 
           (unsigned long)sp, (unsigned long)&__StackBottom, (unsigned long)headroom);

    s_list_cb.fn = callback;
    s_list_cb.user = user;
    return sdcard_list_dir(path, fs_list_dir_callback, NULL);
}

static picocalc_fs_t s_fs_impl = {
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .close = fs_close,
    .exists = fs_exists,
    .size = fs_size,
    .listDir = fs_list_dir,
};

// ── Core 1 entry — background WiFi polling ────────────────────────────────────
// Core 1 drives the Mongoose / CYW43 network stack every 5 ms.
// wifi_poll() acquires display_spi_lock() internally, so the SPI1 bus
// (shared between the LCD and the WiFi chip) is safe to access from here.
// Lua apps benefit automatically; native apps benefit via http_fire_c_pending().

static void core1_entry(void) {
  while (true) {
    wifi_poll();
    http_fire_c_pending();
    sleep_ms(5);
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

  printf("\n--- PicOS booting ---\n");

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

  gpio_init(USB_VBUS_PIN);
  gpio_set_dir(USB_VBUS_PIN, GPIO_IN);

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

  g_api.fs = &s_fs_impl;

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
