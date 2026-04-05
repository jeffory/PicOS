#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/xip.h"
#include "hardware/xip_cache.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "dev_commands.h"
#include "usb/usb_msc.h"

// Forward declarations for display functions used in hardfault_c below.
// The full #include "drivers/display.h" appears later in the file after other
// includes; these declarations let the fault handler use them before that point.
#include <stdint.h>
void display_clear(uint16_t color);
int  display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
void display_flush(void);

// Forward declarations for launcher functions used in hardfault_c.
const char* launcher_get_running_app_name(void);
uint32_t    launcher_get_app_uptime_ms(void);

// Linker symbols for the main stack limits (see boot2/memmap_*.ld)
extern uint32_t __StackTop;    // initial SP (stack grows DOWN from here)
extern uint32_t __StackBottom; // lowest valid address (4KB below StackTop)

// ── HardFault handler ─────────────────────────────────────────────────────────
// Captures the exception frame (stacked registers) and prints fault registers
// to UART+USB so we can identify the crash address.  UART stdio is polling-
// based, so this works even with interrupts disabled inside the fault handler.

// Native app stack base pointer (defined in native_loader.c).
// Dynamically allocated from PSRAM; NULL when no native app is running.
// Used to detect PSP stack overflow in the hardfault handler.
extern uint8_t *g_native_stack_base;

static void __attribute__((used)) hardfault_c(uint32_t *frame, uint32_t exc_return) {
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
  uint32_t ccr = *(volatile uint32_t *)0xE000ED14u;  // SCB CCR

  // Grab app context before anything else (best-effort, pointers may be bad).
  const char *app_name = launcher_get_running_app_name();
  uint32_t uptime_ms = launcher_get_app_uptime_ms();
  uint32_t uptime_sec = uptime_ms / 1000u;

  // Persist fault data in watchdog scratch registers so it survives a
  // watchdog reset and can be dumped to SD on next boot.
  #define CRASH_MAGIC 0xDEAD1234u
  watchdog_hw->scratch[0] = CRASH_MAGIC;
  watchdog_hw->scratch[1] = pc;
  watchdog_hw->scratch[2] = lr;
  watchdog_hw->scratch[3] = (uint32_t)(uintptr_t)frame + 32u; // pre-fault SP
  watchdog_hw->scratch[4] = cfsr;
  watchdog_hw->scratch[5] = hfsr;
  watchdog_hw->scratch[6] = bfar;
  // Pack PSP bit (bit 31) + uptime in seconds (bits 0-30) into scratch[7].
  // EXC_RETURN only needs bit 2 (PSP vs MSP) for crash log decoding.
  watchdog_hw->scratch[7] = ((exc_return & 4u) ? (1u << 31) : 0u)
                           | (uptime_sec & 0x7FFFFFFFu);

  // frame IS the MSP/PSP just after the hardware pushed the 8-word exception
  // frame.  Pre-fault SP = frame + 32 (8 words × 4 bytes).
  uint32_t sp_at_fault = (uint32_t)(uintptr_t)frame + 32u;

  // Determine which stack was active: EXC_RETURN bit 2 = 1 means PSP (native
  // app), 0 means MSP (OS).  Compare SP against the correct stack bounds.
  bool on_psp = (exc_return & 4u) != 0;
  bool stack_overflow;
  uint32_t stack_limit;
  if (on_psp) {
    stack_limit = (uint32_t)(uintptr_t)g_native_stack_base;
    stack_overflow = g_native_stack_base && (sp_at_fault < stack_limit);
  } else {
    stack_limit = (uint32_t)(uintptr_t)&__StackBottom;
    stack_overflow = (sp_at_fault < stack_limit);
  }

  // ── UART output (always works — polling-based, no IRQ required) ────────────
  printf("\n!!! HARDFAULT !!!\n");
  if (app_name)
    printf("  App  = %s (uptime %lum %lus)\n",
           app_name, (unsigned long)(uptime_sec / 60u), (unsigned long)(uptime_sec % 60u));
  else
    printf("  App  = (none -- OS/launcher)\n");
  printf("  PC   = 0x%08lx\n", (unsigned long)pc);
  printf("  LR   = 0x%08lx\n", (unsigned long)lr);
  printf("  R0   = 0x%08lx\n", (unsigned long)r0);
  printf("  SP   = 0x%08lx  (frame @ 0x%08lx)  stack_limit=0x%08lx [%s]%s\n",
         (unsigned long)sp_at_fault, (unsigned long)(uintptr_t)frame,
         (unsigned long)stack_limit,
         on_psp ? "PSP" : "MSP",
         stack_overflow ? " *** OVERFLOW ***" : "");
  printf("  CFSR = 0x%08lx\n", (unsigned long)cfsr);
  printf("  HFSR = 0x%08lx\n", (unsigned long)hfsr);
  printf("  BFAR = 0x%08lx\n", (unsigned long)bfar);
  printf("  MMAR = 0x%08lx\n", (unsigned long)mmar);
  printf("  CCR  = 0x%08lx (bit3=UNALIGNED_TRP)\n", (unsigned long)ccr);
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
  // display_clear/draw_text only write to the SRAM framebuffer (no SPI).
  // display_flush sends the buffer to the LCD via PIO+DMA.
  // lcd_spi_wait_idle() now has a 100ms timeout (C2 fix), so even if the
  // LCD is disconnected or PIO is stuck, we won't hang forever.
  // Uses static buffers to avoid touching an already-damaged stack.
  static char ln[56]; // static to avoid stack usage
  display_clear(0x0000); // black

  snprintf(ln, sizeof(ln), "!!! HARDFAULT !!!");
  display_draw_text(4,  4, ln, 0xF800, 0x0000); // red

  if (app_name)
    snprintf(ln, sizeof(ln), "App: %.28s (%lum %lus)",
             app_name, (unsigned long)(uptime_sec / 60u), (unsigned long)(uptime_sec % 60u));
  else
    snprintf(ln, sizeof(ln), "App: (none -- OS/launcher)");
  display_draw_text(4, 18, ln, 0x07FF, 0x0000); // cyan

  snprintf(ln, sizeof(ln), "PC %08lx  LR %08lx", (unsigned long)pc, (unsigned long)lr);
  display_draw_text(4, 34, ln, 0xFFFF, 0x0000);

  snprintf(ln, sizeof(ln), "SP %08lx  lim %08lx%s",
           (unsigned long)sp_at_fault,
           (unsigned long)stack_limit,
           stack_overflow ? " OVFL!" : "");
  display_draw_text(4, 48, ln, stack_overflow ? 0xF800 : 0xFFFF, 0x0000);

  snprintf(ln, sizeof(ln), "Stack: %s", on_psp ? "PSP (native app)" : "MSP (OS)");
  display_draw_text(4, 62, ln, 0x07E0, 0x0000); // green

  snprintf(ln, sizeof(ln), "CFSR %08lx  HFSR %08lx", (unsigned long)cfsr, (unsigned long)hfsr);
  display_draw_text(4, 76, ln, 0xFFFF, 0x0000);

  snprintf(ln, sizeof(ln), "BFAR %08lx  MMAR %08lx", (unsigned long)bfar, (unsigned long)mmar);
  display_draw_text(4, 90, ln, 0xFFFF, 0x0000);

  // Decode CFSR fault type flags on-screen
  int y = 108;
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
  if (y == 104)         { display_draw_text(4, y, "(no CFSR flags set)",        warn, 0); }

  display_flush();

  // Brief pause so the fault screen is visible before reboot.
  for (volatile int i = 0; i < 2000000; i++) {}

  // Reboot explicitly — watchdog scratch already has the crash data (saved
  // at the top of this function).  On next boot, crash_log_save() writes it
  // to /system/crashlog.txt.
  watchdog_reboot(0, 0, 0);

  // Fallback if reboot doesn't fire immediately.
  while (1) tight_loop_contents();
}

// Naked trampoline: inspect EXC_RETURN to find which stack held the frame,
// then pass its address to the C handler.
void __attribute__((naked)) isr_hardfault(void) {
  __asm volatile (
    "mov  r1, lr     \n" // r1 = EXC_RETURN (2nd arg to hardfault_c)
    "tst  lr, #4     \n" // bit 2 of EXC_RETURN: 0=MSP, 1=PSP
    "ite  eq         \n"
    "mrseq r0, msp   \n" // frame on MSP (normal for thread mode without RTOS)
    "mrsne r0, psp   \n" // frame on PSP (if PSP was active thread stack)
    "b    hardfault_c\n"
  );
}

#include "drivers/audio.h"
#include "drivers/fileplayer.h"
#include "drivers/mp3_player.h"
#include "drivers/pio_psram.h"
#include "drivers/pio_psram_bulk.h"
#include "drivers/sound.h"
#include "drivers/mod_player.h"
#include "drivers/display.h"
#include "drivers/image_api.h"
#include "drivers/video_player.h"
#include "drivers/http.h"
#include "drivers/tcp.h"
#include "drivers/keyboard.h"
#include "drivers/sdcard.h"
#include "drivers/wifi.h"
#include "hardware.h"
#include "os/appconfig.h"
#include "os/config.h"
#include "os/core1_alloc.h"
#include "os/crypto.h"
#include "os/launcher.h"
#include "os/lua_psram_alloc.h"
#include "os/os.h"
#include "os/ota_update.h"
#include "os/perf.h"
#include "os/system_menu.h"
#include "os/toast.h"
#include "os/terminal.h"
#include "os/terminal_render.h"
#include "os/text_input.h"
#include "os/ui.h"
#include "umm_malloc.h"

// ── OS API implementation stubs (wiring the function pointer table)
// ─────────── Full implementations live in each driver. This wires them all
// together into the global g_api struct that Lua and future C apps can
// reference.

PicoCalcAPI g_api;

// Wrapper that composites toast notifications before flushing to display.
// Assigned to g_api.display->flush so all callers (Lua, native, launcher)
// see toasts without modifying their render loops.
static void display_flush_with_toasts(void) {
    toast_draw();
    display_flush();
}

static picocalc_tcp_t s_tcp_impl = {
    .connect = (pctcp_t (*)(const char *, uint16_t, bool))tcp_connect,
    .write = (int (*)(pctcp_t, const void *, int))tcp_write,
    .read = (int (*)(pctcp_t, void *, int))tcp_read,
    .close = (void (*)(pctcp_t))tcp_close,
    .available = (int (*)(pctcp_t))tcp_bytes_available,
    .getError = (const char *(*)(pctcp_t))tcp_get_error,
    .getEvents = (uint32_t (*)(pctcp_t))tcp_take_pending,
};

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
    .drawCircle = display_draw_circle,
    .fillCircle = display_fill_circle,
    .drawText = display_draw_text,
    .flush = display_flush_with_toasts,
    .getWidth = display_get_width_fn,
    .getHeight = display_get_height_fn,
    .setBrightness = display_set_brightness,
    .drawImageNN = display_draw_image_nn,
    .flushRows = display_flush_rows,
    .flushRegion = display_flush_region,
    .getBackBuffer = display_get_back_buffer,
    .effectInvert = display_effect_invert,
    .effectDarken = display_effect_darken,
    .effectBrighten = display_effect_brighten,
    .effectTint = display_effect_tint,
    .effectGrayscale = display_effect_grayscale,
    .effectBlend = display_effect_blend,
    .effectPalette = display_effect_palette,
    .effectDither = display_effect_dither,
    .effectScanline = display_effect_scanline,
    .effectPosterize = display_effect_posterize,
    .fillVLine = display_fill_vline,
    .drawTexturedColumn = display_draw_textured_column,
    .fillVLineGradient = display_fill_vline_gradient,
};

static uint32_t sys_getTimeMs(void) {
  return to_ms_since_boot(get_absolute_time());
}
static uint64_t sys_getTimeUs(void) {
  return time_us_64();
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

// Native-app exit flag: set by the system menu "Exit App" action,
// consumed by sys_shouldExit() which the app checks each frame.
static volatile bool s_native_exit = false;

// Pending app launch from serial command
static const char* s_pending_launch = NULL;

// Native-app tick: poll keyboard, fire pending C HTTP callbacks, and
// check the Sym (Menu) key to show the system menu overlay.
static void sys_poll(void) {
  kbd_poll();
  watchdog_update();
  http_fire_c_pending();
  if (kbd_consume_menu_press()) {
    if (system_menu_show_for_native())
      s_native_exit = true;
  }

  // Poll for serial commands
  dev_commands_poll();
  dev_commands_process();

  if (dev_commands_wants_exit()) {
    s_native_exit = true;
    dev_commands_clear_exit();
  }
  if (dev_commands_wants_reboot()) {
    printf("[DEV] Rebooting...\n");
    stdio_flush();
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);
  }
  if (dev_commands_wants_reboot_flash()) {
    printf("[DEV] Rebooting to BOOTSEL mode...\n");
    stdio_flush();
    sleep_ms(100);
    reset_usb_boot(0, 0);
  }
}

// Check if there's a pending launch request (from serial command)
// Returns app name if pending, NULL otherwise
const char* sys_get_pending_launch(void) {
  return s_pending_launch;
}

void sys_clear_pending_launch(void) {
  s_pending_launch = NULL;
}

static bool sys_shouldExit(void) {
  bool v = s_native_exit;
  s_native_exit = false;
  return v;
}

static void sys_setAudioCallback(void (*cb)(void)) {
  g_native_audio_callback = cb;
}

static picocalc_sys_t s_sys_impl = {
    .getTimeMs = sys_getTimeMs,
    .getTimeUs = sys_getTimeUs,
    .reboot = sys_reboot,
    .getBatteryPercent = kbd_get_battery_percent,
    .isUSBPowered = sys_isUSBPowered,
    .addMenuItem = system_menu_add_item,
    .clearMenuItems = system_menu_clear_items,
    .log = sys_log,
    .poll = sys_poll,
    .shouldExit = sys_shouldExit,
    .setAudioCallback = sys_setAudioCallback,
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
    .startStream = audio_start_stream,
    .stopStream = audio_stop_stream,
    .pushSamples = audio_push_samples,
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
static int fs_fsize(pcfile_t f) {
    return sdcard_fsize_handle((sdfile_t)f);
}
static bool fs_seek(pcfile_t f, uint32_t offset) {
    return sdcard_fseek((sdfile_t)f, offset);
}
static uint32_t fs_tell(pcfile_t f) {
    return sdcard_ftell((sdfile_t)f);
}

// Static-global callback state avoids passing a stack-allocated pointer
// through the deep FatFS call chain (f_opendir → f_readdir → ...).
// A stack pointer can be clobbered by an ISR or a compiler tail-call before
// sdcard_list_dir's callback fires, causing an INVSTATE hard fault.
static struct {
    void (*fn)(const char *, bool, uint32_t, void *);
    void *user;
} s_list_cb;

static void fs_list_dir_callback(const sdcard_entry_t *entry, void *user) {
    (void)user;
    s_list_cb.fn(entry->name, entry->is_dir, entry->size, s_list_cb.user);
}

static int fs_list_dir(const char *path,
                       void (*callback)(const char *name, bool is_dir,
                                        uint32_t size, void *user),
                       void *user) {
    s_list_cb.fn = callback;
    s_list_cb.user = user;
    return sdcard_list_dir(path, fs_list_dir_callback, NULL);
}

static const picocalc_ui_t s_ui_impl = {
    .textInput       = text_input_show,
    .textInputSimple = ui_text_input,
    .confirm         = ui_confirm,
};

// PSRAM wrapper functions
static bool psram_pio_available(void) { return pio_psram_available(); }
static bool psram_pio_bulk_available(void) { return pio_psram_bulk_available(); }
static void psram_pio_read(uint32_t addr, uint8_t *dst, uint32_t len) { pio_psram_read(addr, dst, len); }
static void psram_pio_write(uint32_t addr, const uint8_t *src, uint32_t len) { pio_psram_write(addr, src, len); }
static void psram_pio_bulk_read(uint32_t addr, uint8_t *dst, uint32_t len) { pio_psram_bulk_read_large(addr, dst, len); }
static void psram_pio_bulk_write(uint32_t addr, const uint8_t *src, uint32_t len) { pio_psram_bulk_write_large(addr, src, len); }
static void *psram_qmi_alloc(uint32_t size) { return umm_malloc(size); }
static void psram_qmi_free(void *ptr) { umm_free(ptr); }

static const picocalc_psram_t s_psram_impl = {
    .pioAvailable     = psram_pio_available,
    .pioBulkAvailable = psram_pio_bulk_available,
    .pioRead          = psram_pio_read,
    .pioWrite         = psram_pio_write,
    .pioBulkRead      = psram_pio_bulk_read,
    .pioBulkWrite     = psram_pio_bulk_write,
    .qmiAlloc         = psram_qmi_alloc,
    .qmiFree          = psram_qmi_free,
};

static void perf_draw_fps_wrapper(int x, int y) {
    int fps = perf_get_fps();
    char buf[16];
    snprintf(buf, sizeof(buf), "FPS: %d", fps);
    uint16_t color = (fps >= 55)   ? COLOR_GREEN
                     : (fps >= 30) ? COLOR_YELLOW
                                   : COLOR_RED;
    display_draw_text(x, y, buf, color, COLOR_BLACK);
}

static const picocalc_perf_t s_perf_impl = {
    .beginFrame = perf_begin_frame,
    .endFrame = perf_end_frame,
    .getFPS = perf_get_fps,
    .getFrameTime = perf_get_frame_time,
    .drawFPS = perf_draw_fps_wrapper,
    .setTargetFPS = perf_set_target_fps,
};

static picocalc_terminal_t s_terminal_impl = {
    .create = terminal_new,
    .free = terminal_free,
    .clear = terminal_clear,
    .write = terminal_putString,
    .putChar = terminal_putChar,
    .setCursor = terminal_setCursor,
    .getCursor = terminal_getCursor,
    .setColors = terminal_setColors,
    .getColors = terminal_getColors,
    .scroll = terminal_scroll,
    .render = terminal_render,
    .renderDirty = terminal_renderDirty,
    .getCols = terminal_getCols,
    .getRows = terminal_getRows,
    .setCursorVisible = terminal_setCursorVisible,
    .setCursorBlink = terminal_setCursorBlink,
    .markAllDirty = terminal_markAllDirty,
    .isFullDirty = terminal_isFullDirty,
    .getDirtyRange = terminal_getDirtyRange,
    .getScrollbackCount = terminal_getScrollbackCount,
    .setScrollbackOffset = terminal_setScrollbackOffset,
    .getScrollbackOffset = terminal_getScrollbackOffset,
    .getScrollbackLine = terminal_getScrollbackLine,
    .getScrollbackLineColors = terminal_getScrollbackLineColors,
    // Line numbers
    .setLineNumbers = terminal_setLineNumbers,
    .setLineNumberStart = terminal_setLineNumberStart,
    .setLineNumberCols = terminal_setLineNumberCols,
    .setLineNumberColors = terminal_setLineNumberColors,
    .getContentCols = terminal_getContentCols,
    // Scrollbar
    .setScrollbar = terminal_setScrollbar,
    .setScrollbarColors = terminal_setScrollbarColors,
    .setScrollbarWidth = terminal_setScrollbarWidth,
    .setScrollInfo = terminal_setScrollInfo,
    .setRenderBounds = terminal_setRenderBounds,
    // Word wrap (visual - content not modified)
    .setWordWrap = terminal_setWordWrap,
    .setWordWrapColumn = terminal_setWordWrapColumn,
    .setWrapIndicator = terminal_setWrapIndicator,
    .getWordWrap = terminal_getWordWrap,
    .getVisualRowCount = terminal_getVisualRowCount,
    .logicalToVisual = terminal_logicalToVisual,
    .visualToLogical = terminal_visualToLogical,
    .calculateLineWraps = terminal_calculateLineWraps,
};

static bool fs_mkdir(const char *path) {
    return sdcard_mkdir(path);
}

static bool fs_delete(const char *path) {
    return sdcard_delete(path);
}

static bool fs_rename(const char *src, const char *dst) {
    return sdcard_rename(src, dst);
}

static bool fs_is_dir(const char *path) {
    sdcard_stat_t st;
    return sdcard_stat(path, &st) && st.is_dir;
}

static picocalc_fs_t s_fs_impl = {
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .close = fs_close,
    .exists = fs_exists,
    .size = fs_size,
    .fsize = fs_fsize,
    .seek = fs_seek,
    .tell = fs_tell,
    .listDir = fs_list_dir,
    .mkdir = fs_mkdir,
    .deleteFile = fs_delete,
    .renameFile = fs_rename,
    .isDir = fs_is_dir,
};

// ── HTTP impl ─────────────────────────────────────────────────────────────────

static pchttp_t http_newConn_w(const char *server, uint16_t port, bool use_ssl) {
    http_conn_t *c = http_alloc();
    if (!c) return NULL;
    // Copy server hostname and port/ssl into the connection slot
    strncpy(c->server, server, HTTP_SERVER_MAX - 1);
    c->server[HTTP_SERVER_MAX - 1] = '\0';
    c->port    = port;
    c->use_ssl = use_ssl;
    return (pchttp_t)c;
}

static void http_get_w(pchttp_t c, const char *path, const char *extra_hdrs) {
    http_get((http_conn_t *)c, path, extra_hdrs);
}

static void http_post_w(pchttp_t c, const char *path, const char *extra_hdrs,
                        const char *body, uint32_t body_len) {
    http_post((http_conn_t *)c, path, extra_hdrs, body, (size_t)body_len);
}

static int http_read_w(pchttp_t c, uint8_t *buf, uint32_t len) {
    return (int)http_read((http_conn_t *)c, buf, len);
}

static uint32_t http_available_w(pchttp_t c) {
    return http_bytes_available((http_conn_t *)c);
}

static void http_close_w(pchttp_t c) {
    http_free((http_conn_t *)c);
}

static int http_getStatus_w(pchttp_t c) {
    return ((http_conn_t *)c)->status_code;
}

static const char *http_getError_w(pchttp_t c) {
    http_conn_t *hc = (http_conn_t *)c;
    return hc->err[0] ? hc->err : NULL;
}

static int http_getProgress_w(pchttp_t c, int *received, int *total) {
    http_conn_t *hc = (http_conn_t *)c;
    if (received) *received = (int)hc->body_received;
    if (total)    *total    = (int)hc->content_length;  // -1 if unknown
    return (int)hc->content_length;
}

static void http_setKeepAlive_w(pchttp_t c, bool keep_alive) {
    ((http_conn_t *)c)->keep_alive = keep_alive;
}

static void http_setByteRange_w(pchttp_t c, int from, int to) {
    ((http_conn_t *)c)->range_from = from;
    ((http_conn_t *)c)->range_to   = to;
}

static void http_setConnectTimeout_w(pchttp_t c, int seconds) {
    ((http_conn_t *)c)->connect_timeout_ms = (uint32_t)(seconds * 1000);
}

static void http_setReadTimeout_w(pchttp_t c, int seconds) {
    ((http_conn_t *)c)->read_timeout_ms = (uint32_t)(seconds * 1000);
}

static bool http_setReadBufferSize_w(pchttp_t c, int bytes) {
    return http_set_recv_buf((http_conn_t *)c, (uint32_t)bytes);
}

static bool http_isComplete_w(pchttp_t c) {
    http_conn_t *hc = (http_conn_t *)c;
    return hc->state == HTTP_STATE_DONE || hc->state == HTTP_STATE_FAILED;
}

static const picocalc_http_t s_http_impl = {
    .newConn           = http_newConn_w,
    .get               = http_get_w,
    .post              = http_post_w,
    .read              = http_read_w,
    .available         = http_available_w,
    .close             = http_close_w,
    .getStatus         = http_getStatus_w,
    .getError          = http_getError_w,
    .getProgress       = http_getProgress_w,
    .setKeepAlive      = http_setKeepAlive_w,
    .setByteRange      = http_setByteRange_w,
    .setConnectTimeout = http_setConnectTimeout_w,
    .setReadTimeout    = http_setReadTimeout_w,
    .setReadBufferSize = http_setReadBufferSize_w,
    .isComplete        = http_isComplete_w,
};

// ── Sound player impl ─────────────────────────────────────────────────────────

static pcsound_sample_t sp_sampleLoad(const char *path) {
    sound_sample_t *s = sound_sample_create();
    if (!s) return NULL;
    if (!sound_sample_load(s, path)) { sound_sample_destroy(s); return NULL; }
    return (pcsound_sample_t)s;
}

static void sp_sampleFree(pcsound_sample_t s) {
    sound_sample_destroy((sound_sample_t *)s);
}

static pcsound_player_t sp_playerNew(void) {
    return (pcsound_player_t)sound_player_create();
}

static void sp_playerSetSample(pcsound_player_t p, pcsound_sample_t s) {
    sound_player_set_sample((sound_player_t *)p, (sound_sample_t *)s);
}

static void sp_playerPlay(pcsound_player_t p, uint8_t repeat_count) {
    sound_player_play((sound_player_t *)p, repeat_count);
}

static void sp_playerStop(pcsound_player_t p) {
    sound_player_stop((sound_player_t *)p);
}

static bool sp_playerIsPlaying(pcsound_player_t p) {
    return sound_player_is_playing((const sound_player_t *)p);
}

static uint8_t sp_playerGetVolume(pcsound_player_t p) {
    return sound_player_get_volume((const sound_player_t *)p);
}

static void sp_playerSetVolume(pcsound_player_t p, uint8_t vol) {
    sound_player_set_volume((sound_player_t *)p, vol);
}

// sound.h has no sound_player_set_loop; repeat_count=0 with playerPlay means
// the player won't loop — callers should use repeat_count to control looping.
// We store loop intent as repeat_count=255 (max repeats) when loop=true,
// and stop the player when loop=false.
static void sp_playerSetLoop(pcsound_player_t p, bool loop) {
    sound_player_t *sp = (sound_player_t *)p;
    sp->repeat_count = loop ? 255 : 0;
}

static void sp_playerFree(pcsound_player_t p) {
    sound_player_destroy((sound_player_t *)p);
}

static pcfileplayer_t sp_filePlayerNew(void) {
    return (pcfileplayer_t)fileplayer_create();
}

static void sp_filePlayerLoad(pcfileplayer_t fp, const char *path) {
    fileplayer_load((fileplayer_t *)fp, path);
}

static void sp_filePlayerPlay(pcfileplayer_t fp, uint8_t repeat_count) {
    fileplayer_play((fileplayer_t *)fp, repeat_count);
}

static void sp_filePlayerStop(pcfileplayer_t fp) {
    fileplayer_stop((fileplayer_t *)fp);
}

static void sp_filePlayerPause(pcfileplayer_t fp) {
    fileplayer_pause((fileplayer_t *)fp);
}

static void sp_filePlayerResume(pcfileplayer_t fp) {
    fileplayer_resume((fileplayer_t *)fp);
}

static bool sp_filePlayerIsPlaying(pcfileplayer_t fp) {
    return fileplayer_is_playing((const fileplayer_t *)fp);
}

static void sp_filePlayerSetVolume(pcfileplayer_t fp, uint8_t vol) {
    fileplayer_set_volume((fileplayer_t *)fp, vol, vol);
}

static uint8_t sp_filePlayerGetVolume(pcfileplayer_t fp) {
    uint8_t left = 0, right = 0;
    fileplayer_get_volume((const fileplayer_t *)fp, &left, &right);
    return left;
}

static uint32_t sp_filePlayerGetOffset(pcfileplayer_t fp) {
    return fileplayer_get_offset((const fileplayer_t *)fp);
}

static void sp_filePlayerSetOffset(pcfileplayer_t fp, uint32_t pos) {
    fileplayer_set_offset((fileplayer_t *)fp, pos);
}

static bool sp_filePlayerDidUnderrun(pcfileplayer_t fp) {
    (void)fp;
    return fileplayer_did_underrun();
}

static void sp_filePlayerFree(pcfileplayer_t fp) {
    fileplayer_destroy((fileplayer_t *)fp);
}

static pcmp3player_t sp_mp3PlayerNew(void) {
    return (pcmp3player_t)mp3_player_create();
}

static void sp_mp3PlayerLoad(pcmp3player_t mp, const char *path) {
    mp3_player_load((mp3_player_t *)mp, path);
}

static void sp_mp3PlayerPlay(pcmp3player_t mp, uint8_t repeat_count) {
    mp3_player_play((mp3_player_t *)mp, repeat_count);
}

static void sp_mp3PlayerStop(pcmp3player_t mp) {
    mp3_player_stop((mp3_player_t *)mp);
}

static void sp_mp3PlayerPause(pcmp3player_t mp) {
    mp3_player_pause((mp3_player_t *)mp);
}

static void sp_mp3PlayerResume(pcmp3player_t mp) {
    mp3_player_resume((mp3_player_t *)mp);
}

static bool sp_mp3PlayerIsPlaying(pcmp3player_t mp) {
    return mp3_player_is_playing((const mp3_player_t *)mp);
}

static void sp_mp3PlayerSetVolume(pcmp3player_t mp, uint8_t vol) {
    mp3_player_set_volume((mp3_player_t *)mp, vol);
}

static uint8_t sp_mp3PlayerGetVolume(pcmp3player_t mp) {
    return mp3_player_get_volume((const mp3_player_t *)mp);
}

static void sp_mp3PlayerSetLoop(pcmp3player_t mp, bool loop) {
    mp3_player_set_loop((mp3_player_t *)mp, loop);
}

static void sp_mp3PlayerFree(pcmp3player_t mp) {
    mp3_player_destroy((mp3_player_t *)mp);
}

static const picocalc_soundplayer_t s_soundplayer_impl = {
    .sampleLoad          = sp_sampleLoad,
    .sampleFree          = sp_sampleFree,
    .playerNew           = sp_playerNew,
    .playerSetSample     = sp_playerSetSample,
    .playerPlay          = sp_playerPlay,
    .playerStop          = sp_playerStop,
    .playerIsPlaying     = sp_playerIsPlaying,
    .playerGetVolume     = sp_playerGetVolume,
    .playerSetVolume     = sp_playerSetVolume,
    .playerSetLoop       = sp_playerSetLoop,
    .playerFree          = sp_playerFree,
    .filePlayerNew       = sp_filePlayerNew,
    .filePlayerLoad      = sp_filePlayerLoad,
    .filePlayerPlay      = sp_filePlayerPlay,
    .filePlayerStop      = sp_filePlayerStop,
    .filePlayerPause     = sp_filePlayerPause,
    .filePlayerResume    = sp_filePlayerResume,
    .filePlayerIsPlaying = sp_filePlayerIsPlaying,
    .filePlayerSetVolume = sp_filePlayerSetVolume,
    .filePlayerGetVolume = sp_filePlayerGetVolume,
    .filePlayerGetOffset = sp_filePlayerGetOffset,
    .filePlayerSetOffset = sp_filePlayerSetOffset,
    .filePlayerDidUnderrun = sp_filePlayerDidUnderrun,
    .filePlayerFree      = sp_filePlayerFree,
    .mp3PlayerNew        = sp_mp3PlayerNew,
    .mp3PlayerLoad       = sp_mp3PlayerLoad,
    .mp3PlayerPlay       = sp_mp3PlayerPlay,
    .mp3PlayerStop       = sp_mp3PlayerStop,
    .mp3PlayerPause      = sp_mp3PlayerPause,
    .mp3PlayerResume     = sp_mp3PlayerResume,
    .mp3PlayerIsPlaying  = sp_mp3PlayerIsPlaying,
    .mp3PlayerSetVolume  = sp_mp3PlayerSetVolume,
    .mp3PlayerGetVolume  = sp_mp3PlayerGetVolume,
    .mp3PlayerSetLoop    = sp_mp3PlayerSetLoop,
    .mp3PlayerFree       = sp_mp3PlayerFree,
};

// ── App config impl ───────────────────────────────────────────────────────────

static const picocalc_appconfig_t s_appconfig_impl = {
    .load     = appconfig_load,
    .save     = appconfig_save,
    .get      = appconfig_get,
    .set      = appconfig_set,
    .clear    = appconfig_clear,
    .reset    = appconfig_reset,
    .getAppId = appconfig_get_app_id,
};

// ── Crypto impl ───────────────────────────────────────────────────────────────

static pccrypto_aes_t crypto_aes_new_w(const uint8_t *key, uint32_t klen,
                                        const uint8_t *nonce) {
    return (pccrypto_aes_t)crypto_aes_new(key, klen, nonce);
}

static int crypto_aes_update_w(pccrypto_aes_t ctx,
                                const uint8_t *in, uint8_t *out, uint32_t len) {
    return crypto_aes_update((crypto_aes_t *)ctx, in, out, len);
}

static void crypto_aes_free_w(pccrypto_aes_t ctx) {
    crypto_aes_free((crypto_aes_t *)ctx);
}

static pccrypto_ecdh_t crypto_ecdh_x25519_w(void) {
    return (pccrypto_ecdh_t)crypto_ecdh_x25519();
}

static pccrypto_ecdh_t crypto_ecdh_p256_w(void) {
    return (pccrypto_ecdh_t)crypto_ecdh_p256();
}

static void crypto_ecdh_get_pubkey_w(pccrypto_ecdh_t ctx,
                                      uint8_t *out, uint32_t *out_len) {
    crypto_ecdh_get_public_key((crypto_ecdh_t *)ctx, out, out_len);
}

static int crypto_ecdh_shared_w(pccrypto_ecdh_t ctx,
                                 const uint8_t *remote, uint32_t rlen,
                                 uint8_t *out, uint32_t *out_len) {
    return crypto_ecdh_compute_shared((crypto_ecdh_t *)ctx, remote, rlen,
                                      out, out_len);
}

static void crypto_ecdh_free_w(pccrypto_ecdh_t ctx) {
    crypto_ecdh_free((crypto_ecdh_t *)ctx);
}

static const picocalc_crypto_t s_crypto_impl = {
    .sha256            = crypto_sha256,
    .sha1              = crypto_sha1,
    .hmacSha256        = crypto_hmac_sha256,
    .hmacSha1          = crypto_hmac_sha1,
    .randomBytes       = crypto_random_bytes,
    .deriveKey         = crypto_derive_key,
    .aesNew            = crypto_aes_new_w,
    .aesUpdate         = crypto_aes_update_w,
    .aesFree           = crypto_aes_free_w,
    .ecdhX25519        = crypto_ecdh_x25519_w,
    .ecdhP256          = crypto_ecdh_p256_w,
    .ecdhGetPublicKey  = crypto_ecdh_get_pubkey_w,
    .ecdhComputeShared = crypto_ecdh_shared_w,
    .ecdhFree          = crypto_ecdh_free_w,
    .rsaVerify         = crypto_rsa_verify,
    .ecdsaP256Verify   = crypto_ecdsa_p256_verify,
};

// ── Graphics impl ─────────────────────────────────────────────────────────────

static pcimage_t gfx_load(const char *path) {
    return (pcimage_t)image_load(path);
}
static pcimage_t gfx_new_blank(int w, int h) {
    return (pcimage_t)image_new_blank(w, h);
}
static void gfx_free(pcimage_t img) {
    image_free((pc_image_t *)img);
}
static int gfx_width(pcimage_t img) {
    return ((pc_image_t *)img)->w;
}
static int gfx_height(pcimage_t img) {
    return ((pc_image_t *)img)->h;
}
static uint16_t *gfx_pixels(pcimage_t img) {
    return ((pc_image_t *)img)->data;
}
static void gfx_set_transparent_color(pcimage_t img, uint16_t color) {
    ((pc_image_t *)img)->transparent_color = color;
}
static void gfx_draw(pcimage_t img, int x, int y) {
    image_draw((const pc_image_t *)img, x, y);
}
static void gfx_draw_region(pcimage_t img, int sx, int sy, int sw, int sh,
                             int dx, int dy) {
    image_draw_region((const pc_image_t *)img, sx, sy, sw, sh, dx, dy);
}
static void gfx_draw_scaled(pcimage_t img, int x, int y, int dst_w, int dst_h) {
    image_draw_scaled((const pc_image_t *)img, x, y, dst_w, dst_h);
}

static const picocalc_graphics_t s_graphics_impl = {
    .load                = gfx_load,
    .newBlank            = gfx_new_blank,
    .free                = gfx_free,
    .width               = gfx_width,
    .height              = gfx_height,
    .pixels              = gfx_pixels,
    .setTransparentColor = gfx_set_transparent_color,
    .draw                = gfx_draw,
    .drawRegion          = gfx_draw_region,
    .drawScaled          = gfx_draw_scaled,
};

// ── Video impl ────────────────────────────────────────────────────────────────

static pcvideo_t video_new_player(void) {
    return (pcvideo_t)video_player_create();
}
static void video_free_player(pcvideo_t vp) {
    video_player_destroy((video_player_t *)vp);
}
static bool video_load_w(pcvideo_t vp, const char *path) {
    return video_player_load((video_player_t *)vp, path);
}
static void video_play_w(pcvideo_t vp) {
    video_player_play((video_player_t *)vp);
}
static void video_pause_w(pcvideo_t vp) {
    video_player_pause((video_player_t *)vp);
}
static void video_resume_w(pcvideo_t vp) {
    video_player_resume((video_player_t *)vp);
}
static void video_stop_w(pcvideo_t vp) {
    video_player_stop((video_player_t *)vp);
}
static bool video_update_w(pcvideo_t vp) {
    return video_player_update((video_player_t *)vp);
}
static void video_seek_w(pcvideo_t vp, uint32_t frame) {
    video_player_seek((video_player_t *)vp, frame);
}
static float video_get_fps_w(pcvideo_t vp) {
    return video_player_get_fps((video_player_t *)vp);
}
static void video_get_size_w(pcvideo_t vp, uint32_t *w, uint32_t *h) {
    video_player_t *p = (video_player_t *)vp;
    if (w) *w = p->width;
    if (h) *h = p->height;
}
static bool video_is_playing_w(pcvideo_t vp) {
    video_player_t *p = (video_player_t *)vp;
    return p->playing && !p->paused;
}
static bool video_is_paused_w(pcvideo_t vp) {
    return ((video_player_t *)vp)->paused;
}
static void video_set_loop_w(pcvideo_t vp, bool loop) {
    ((video_player_t *)vp)->loop = loop;
}
static void video_set_auto_flush_w(pcvideo_t vp, bool af) {
    ((video_player_t *)vp)->auto_flush = af;
}
static bool video_has_audio_w(pcvideo_t vp) {
    return video_player_has_audio((video_player_t *)vp);
}
static void video_set_volume_w(pcvideo_t vp, uint8_t vol) {
    video_player_set_audio_volume((video_player_t *)vp, vol);
}
static uint8_t video_get_volume_w(pcvideo_t vp) {
    return video_player_get_audio_volume((video_player_t *)vp);
}
static void video_set_muted_w(pcvideo_t vp, bool muted) {
    video_player_set_audio_muted((video_player_t *)vp, muted);
}
static bool video_get_muted_w(pcvideo_t vp) {
    return video_player_get_audio_muted((video_player_t *)vp);
}
static uint32_t video_get_dropped_w(pcvideo_t vp) {
    return video_player_get_dropped_frames((video_player_t *)vp);
}
static void video_reset_stats_w(pcvideo_t vp) {
    video_player_reset_stats((video_player_t *)vp);
}

static const picocalc_video_t s_video_impl = {
    .newPlayer       = video_new_player,
    .free            = video_free_player,
    .load            = video_load_w,
    .play            = video_play_w,
    .pause           = video_pause_w,
    .resume          = video_resume_w,
    .stop            = video_stop_w,
    .update          = video_update_w,
    .seek            = video_seek_w,
    .getFPS          = video_get_fps_w,
    .getSize         = video_get_size_w,
    .isPlaying       = video_is_playing_w,
    .isPaused        = video_is_paused_w,
    .setLoop         = video_set_loop_w,
    .setAutoFlush    = video_set_auto_flush_w,
    .hasAudio        = video_has_audio_w,
    .setVolume       = video_set_volume_w,
    .getVolume       = video_get_volume_w,
    .setMuted        = video_set_muted_w,
    .getMuted        = video_get_muted_w,
    .getDroppedFrames = video_get_dropped_w,
    .resetStats      = video_reset_stats_w,
};

// ── MOD player wrappers (opaque void* API) ───────────────────────────────────
static pcmodplayer_t mod_create_w(void) { return mod_player_create(); }
static void mod_destroy_w(pcmodplayer_t mp) { mod_player_destroy((mod_player_t *)mp); }
static bool mod_load_w(pcmodplayer_t mp, const char *p) { return mod_player_load((mod_player_t *)mp, p); }
static void mod_play_w(pcmodplayer_t mp, bool loop) { mod_player_play((mod_player_t *)mp, loop); }
static void mod_stop_w(pcmodplayer_t mp) { mod_player_stop((mod_player_t *)mp); }
static void mod_pause_w(pcmodplayer_t mp) { mod_player_pause((mod_player_t *)mp); }
static void mod_resume_w(pcmodplayer_t mp) { mod_player_resume((mod_player_t *)mp); }
static bool mod_is_playing_w(pcmodplayer_t mp) { return mod_player_is_playing((const mod_player_t *)mp); }
static void mod_set_volume_w(pcmodplayer_t mp, uint8_t v) { mod_player_set_volume((mod_player_t *)mp, v); }
static uint8_t mod_get_volume_w(pcmodplayer_t mp) { return mod_player_get_volume((const mod_player_t *)mp); }
static void mod_set_loop_w(pcmodplayer_t mp, bool l) { mod_player_set_loop((mod_player_t *)mp, l); }

static const picocalc_modplayer_t s_modplayer_impl = {
    .create    = mod_create_w,
    .destroy   = mod_destroy_w,
    .load      = mod_load_w,
    .play      = mod_play_w,
    .stop      = mod_stop_w,
    .pause     = mod_pause_w,
    .resume    = mod_resume_w,
    .isPlaying = mod_is_playing_w,
    .setVolume = mod_set_volume_w,
    .getVolume = mod_get_volume_w,
    .setLoop   = mod_set_loop_w,
};

// ── ZIP extraction (thin wrappers for g_api — Lua bridge has its own richer API) ──

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
// Redirect miniz allocations to PSRAM (umm_malloc), not tiny SRAM heap
#define MZ_MALLOC(x)     umm_malloc(x)
#define MZ_FREE(x)       umm_free(x)
#define MZ_REALLOC(p, x) umm_realloc(p, x)
#include "miniz.h"

static bool zip_extract_w(const char *zip_path, const char *dest_dir) {
    int zip_len = 0;
    char *zip_data = sdcard_read_file(zip_path, &zip_len);
    if (!zip_data) return false;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zip_data, (size_t)zip_len, 0)) {
        umm_free(zip_data);
        return false;
    }

    bool ok = true;
    int n = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < n && ok; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, (mz_uint)i)) continue;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, (mz_uint)i, &st)) { ok = false; break; }

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dest_dir, st.m_filename);

        size_t uncomp = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)i, &uncomp, 0);
        if (!data) { ok = false; break; }

        sdcard_mkdir(dest_dir);  // ensure dest exists
        sdfile_t f = sdcard_fopen(path, "w");
        if (f) {
            sdcard_fwrite(f, data, (int)uncomp);
            sdcard_fclose(f);
        } else { ok = false; }
        mz_free(data);
    }

    mz_zip_reader_end(&zip);
    umm_free(zip_data);
    return ok;
}

static int zip_list_w(const char *zip_path) {
    int zip_len = 0;
    char *zip_data = sdcard_read_file(zip_path, &zip_len);
    if (!zip_data) return -1;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zip_data, (size_t)zip_len, 0)) {
        umm_free(zip_data);
        return -1;
    }
    int n = (int)mz_zip_reader_get_num_files(&zip);
    mz_zip_reader_end(&zip);
    umm_free(zip_data);
    return n;
}

static const picocalc_zip_t s_zip_impl = {
    .extract = zip_extract_w,
    .list    = zip_list_w,
};

// ── Core 1 entry — background WiFi polling ──────��─────────────────────────────
// Core 1 drives the Mongoose / CYW43 network stack every 5 ms.
// wifi_poll() acquires display_spi_lock() internally, so the SPI1 bus
// (shared between the LCD and the WiFi chip) is safe to access from here.
// Lua apps benefit automatically; native apps benefit via http_fire_c_pending().

// Set to true to temporarily pause Core 1's Mongoose/WiFi polling.
// Used during native app loading to eliminate PSRAM heap contention between
// Core 1's umm_malloc/umm_free and the ELF loader on Core 0.
_Atomic bool g_core1_pause = false;
_Atomic bool g_core1_paused = false; // acknowledgment flag for Core 1 pause handshake

// Optional audio callback for native apps (e.g. DOOM) that offload mixing
// to Core 1.  Set by the app at startup, cleared on exit.
_Atomic(void (*)(void)) g_native_audio_callback = NULL;

static repeating_timer_t s_core1_timer;
static volatile bool s_core1_tick_pending = false;

static bool core1_timer_callback(repeating_timer_t *rt) {
  (void)rt;
  s_core1_tick_pending = true;
  return true;
}

// Doorbell ISR: Core 0 rings WIFI_IPC_DOORBELL after pushing to the IPC
// queue.  This wakes Core 1 from __wfi() immediately (<1us latency)
// instead of waiting for the 5ms polling timer.
static void core1_doorbell_isr(void) {
  multicore_doorbell_clear_current_core(WIFI_IPC_DOORBELL);
  s_core1_tick_pending = true;
}

static void core1_entry(void) {
  volatile uint32_t *scb_ccr = (volatile uint32_t *)(0xE000ED14);
  *scb_ccr &= ~(1u << 3);
  __asm volatile ("dsb sy" ::: "memory");
  __asm volatile ("isb sy" ::: "memory");

  audio_core1_init();

  // Register doorbell ISR for instant IPC wake-up from Core 0
  uint doorbell_irq = multicore_doorbell_irq_num(WIFI_IPC_DOORBELL);
  irq_set_exclusive_handler(doorbell_irq, core1_doorbell_isr);
  irq_set_enabled(doorbell_irq, true);

  alarm_pool_t *pool = audio_get_core1_alarm_pool();
  alarm_pool_add_repeating_timer_ms(pool, -1, core1_timer_callback, NULL, &s_core1_timer);

  while (true) {
    if (atomic_load(&g_core1_pause)) {
      atomic_store(&g_core1_paused, true);
      __dmb(); // ensure paused flag visible to Core 0 before we spin
      while (atomic_load(&g_core1_pause)) {
        watchdog_update(); // keep watchdog alive while paused
        sleep_ms(1);
      }
      atomic_store(&g_core1_paused, false);
      __dmb(); // ensure resumed state visible to Core 0
      continue;
    }

    if (s_core1_tick_pending) {
      s_core1_tick_pending = false;

      wifi_poll();
      http_fire_c_pending();

      audio_stream_poll();
      mp3_player_update();
      fileplayer_update();
      mod_player_update();
      void (*audio_cb)(void) = atomic_load(&g_native_audio_callback);
      if (audio_cb)
        audio_cb();
    }

    __wfi();
  }
}

// ── Main ─────────────────────────────────────────────────────────────────────

// ── Crash persistence across watchdog resets ─────────────────────────────────
#define CRASH_MAGIC 0xDEAD1234u
static uint32_t s_crash_data[8];
static bool s_had_crash = false;

static void crash_log_save(void) {
  if (!s_had_crash) return;

  // Truncate if log is too large (>64KB)
  int size = sdcard_fsize("/system/crashlog.txt");
  if (size > 0 && (uint32_t)size > 64u * 1024u) {
    sdfile_t tf = sdcard_fopen("/system/crashlog.txt", "w");
    if (tf) sdcard_fclose(tf);
  }

  sdfile_t f = sdcard_fopen("/system/crashlog.txt", "a");
  if (!f) return;

  uint32_t cfsr = s_crash_data[4];
  uint32_t hfsr = s_crash_data[5];
  // scratch[7] packing: bit 31 = PSP flag, bits 0-30 = uptime in seconds
  bool was_psp = (s_crash_data[7] & (1u << 31)) != 0;
  uint32_t crash_uptime_sec = s_crash_data[7] & 0x7FFFFFFFu;

  char line[512];
  int n = snprintf(line, sizeof(line),
    "--- HARDFAULT ---\n"
    "  Uptime: %lum %lus\n"
    "  PC   = 0x%08lx\n"
    "  LR   = 0x%08lx\n"
    "  SP   = 0x%08lx\n"
    "  CFSR = 0x%08lx\n"
    "  HFSR = 0x%08lx\n"
    "  BFAR = 0x%08lx\n"
    "  Stack: %s\n",
    (unsigned long)(crash_uptime_sec / 60u), (unsigned long)(crash_uptime_sec % 60u),
    (unsigned long)s_crash_data[1], (unsigned long)s_crash_data[2],
    (unsigned long)s_crash_data[3], (unsigned long)cfsr,
    (unsigned long)hfsr, (unsigned long)s_crash_data[6],
    was_psp ? "PSP (native app)" : "MSP (OS)");

  // Decode CFSR/HFSR flags into human-readable text
  if (cfsr & (1u<<17)) n += snprintf(line+n, sizeof(line)-n, "  INVSTATE: invalid CPU state\n");
  if (cfsr & (1u<<16)) n += snprintf(line+n, sizeof(line)-n, "  UNDEFINSTR: undefined instruction\n");
  if (cfsr & (1u<<18)) n += snprintf(line+n, sizeof(line)-n, "  INVPC: invalid EXC_RETURN/PC\n");
  if (cfsr & (1u<<19)) n += snprintf(line+n, sizeof(line)-n, "  NOCP: coprocessor access\n");
  if (cfsr & (1u<< 9)) n += snprintf(line+n, sizeof(line)-n, "  PRECISERR: precise data bus fault\n");
  if (cfsr & (1u<< 8)) n += snprintf(line+n, sizeof(line)-n, "  IBUSERR: instruction bus fault\n");
  if (cfsr & (1u<<10)) n += snprintf(line+n, sizeof(line)-n, "  IMPRECISERR: imprecise data bus fault\n");
  if (cfsr & (1u<<12)) n += snprintf(line+n, sizeof(line)-n, "  STKERR: exception stack push fault\n");
  if (cfsr & (1u<<11)) n += snprintf(line+n, sizeof(line)-n, "  UNSTKERR: exception stack pop fault\n");
  if (cfsr & (1u<< 1)) n += snprintf(line+n, sizeof(line)-n, "  DACCVIOL: MPU data access violation\n");
  if (cfsr & (1u<< 0)) n += snprintf(line+n, sizeof(line)-n, "  IACCVIOL: MPU instruction access violation\n");
  if (cfsr & (1u<<25)) n += snprintf(line+n, sizeof(line)-n, "  DIVBYZERO\n");
  if (cfsr & (1u<<24)) n += snprintf(line+n, sizeof(line)-n, "  UNALIGNED access\n");
  if (hfsr & (1u<<30)) n += snprintf(line+n, sizeof(line)-n, "  HFSR: FORCED escalation\n");
  if (hfsr & (1u<< 1)) n += snprintf(line+n, sizeof(line)-n, "  HFSR: vector table fault\n");
  n += snprintf(line+n, sizeof(line)-n, "\n");

  sdcard_fwrite(f, line, n);
  sdcard_fclose(f);
  printf("[CRASH] Saved crash log to /system/crashlog.txt\n");
}

int main(void) {
  // ── Boot-loop detection using watchdog scratch[0] ──────────────────────────
  // scratch[0] encoding:
  //   CRASH_MAGIC (0xDEAD1234) = HardFault data present
  //   BOOT_MAGIC  (0xB007xx00) = boot attempt counter (low byte = count)
  //   anything else            = fresh boot (power-on or clean reset)
  #define BOOT_MAGIC_MASK  0xFFFFFF00u
  #define BOOT_MAGIC       0xB0070000u
  #define BOOT_MAX_RETRIES 3

  uint32_t scratch0 = watchdog_hw->scratch[0];
  int boot_attempt = 0;
  bool skip_boot_watchdog = false;

  if (scratch0 == CRASH_MAGIC) {
    // HardFault crash recovery — preserve fault data for later display
    memcpy(s_crash_data, (void *)watchdog_hw->scratch, sizeof(s_crash_data));
    s_had_crash = true;
  } else if ((scratch0 & BOOT_MAGIC_MASK) == BOOT_MAGIC) {
    // Previous boot failed during init — increment attempt counter
    boot_attempt = (scratch0 & 0xFF) + 1;
    if (boot_attempt >= BOOT_MAX_RETRIES) {
      // Too many boot failures — disable watchdog so device stays on
      // error screen instead of looping.  User can read the message
      // and power-cycle / reinsert SD.
      skip_boot_watchdog = true;
      boot_attempt = 0; // reset for next power cycle
    }
  } else if (watchdog_caused_reboot()) {
    // Watchdog timeout without HardFault or boot counter (legacy path)
    printf("[WATCHDOG] Reset due to timeout (no fault data)\n");
  }

  // Write boot attempt counter — cleared once launcher starts successfully
  watchdog_hw->scratch[0] = BOOT_MAGIC | (boot_attempt & 0xFF);

  // Overclock to 200 MHz for better display throughput (RP2350 supports 150+)
  // NOTE: If the keyboard fails to initialise reliably, try commenting this
  // out to test at the default 125 MHz — it isolates whether the overclock
  // is affecting I2C timing.
  set_sys_clock_khz(200000, false);

  // Configure peripheral clock to 125 MHz (enables 62.5 MHz SPI for LCD)
  // clk_peri drives UART, SPI, I2C, PWM — ST7789 rated max is 62.5 MHz
  clock_configure(
      clk_peri,
      0,                                                // No glitchless mux
      CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, // Source: PLL_SYS
                                                        // (200MHz)
      200 * MHZ,                                        // Input frequency
      200 * MHZ); // Output: 200 MHz → SPI can reach 100 MHz

  // Enable lazy floating-point context save.  Without this, any ISR that
  // *might* touch FP registers stacks the full 32-register FP context (128
  // bytes) on every entry.  With LSPEN+ASPEN the context is only saved if
  // the ISR actually executes an FP instruction, saving ~12 cycles on ISR
  // entry for the common case (audio DMA, keyboard, etc.).
  // FPCCR: bit 31 = ASPEN (automatic state preservation),
  //        bit 30 = LSPEN (lazy stacking enable)
  volatile uint32_t *fpccr = (volatile uint32_t *)0xE000EF34u;
  *fpccr |= (1u << 31) | (1u << 30);

  stdio_init_all();

  // Wait up to 3 s for a USB serial host to connect so early printf output
  // isn't lost. Skips automatically if already connected.
  for (int i = 0; i < 30 && !stdio_usb_connected(); i++) {
    watchdog_update();
    sleep_ms(100);
  }
  watchdog_update();

  printf("\n--- PicOS booting ---\n");
  printf("[BOOT] scratch0_at_entry=0x%08lx s_had_crash=%d boot_attempt=%d\n",
         (unsigned long)scratch0, s_had_crash, boot_attempt);
  printf("[BOOT] watchdog_caused_reboot=%d\n", watchdog_caused_reboot());

  // Wire up the global API struct
  g_api.input = &s_input_impl;
  g_api.display = &s_display_impl;
  g_api.sys = &s_sys_impl;
  g_api.wifi = &s_wifi_impl;
  g_api.audio = &s_audio_impl;
  g_api.tcp = &s_tcp_impl;
  g_api.ui = &s_ui_impl;
  g_api.psram = &s_psram_impl;
  g_api.perf = &s_perf_impl;
  g_api.terminal    = &s_terminal_impl;
  g_api.http        = &s_http_impl;
  g_api.soundplayer = &s_soundplayer_impl;
  g_api.appconfig   = &s_appconfig_impl;
  g_api.crypto      = &s_crypto_impl;
  g_api.graphics    = &s_graphics_impl;
  g_api.video       = &s_video_impl;
  g_api.modplayer   = &s_modplayer_impl;
  g_api.zip         = &s_zip_impl;
  g_api.version     = 2;
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

  // Arm watchdog early so any boot hang triggers a reset.
  // Disabled after BOOT_MAX_RETRIES failures so the device stays on the
  // error screen instead of looping forever.
  if (!skip_boot_watchdog)
    watchdog_enable(10000, true); // 10s, pause on debug

  // Initialise mainboard PIO PSRAM (8MB on PIO1, independent of QMI PSRAM).
  // Non-fatal if chip not present (some boards may not have it).
  // Uses bulk driver internally for 8KB transfers (300x faster than 27-byte chunks).
  pio_psram_init();
  printf("[BOOT] pio_psram done\n"); stdio_flush(); watchdog_update();

  sound_init();
  printf("[BOOT] sound done\n"); stdio_flush(); watchdog_update();

  audio_init();
  printf("[BOOT] audio done\n"); stdio_flush(); watchdog_update();

  mod_player_init();
  printf("[BOOT] mod_player done\n"); stdio_flush(); watchdog_update();

  printf("[BOOT] drawing splash...\n"); stdio_flush();
  ui_draw_splash("Initialising keyboard...", NULL);
  printf("[BOOT] splash done\n"); stdio_flush(); watchdog_update();

  bool kbd_ok = kbd_init();
  watchdog_update();
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
    watchdog_update();
    sleep_ms(5000);
  }

  ui_draw_splash("Mounting SD card...", NULL);
  watchdog_update();
  bool sd_ok = sdcard_init();
  watchdog_update();

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
      watchdog_update();
      if (kbd_get_buttons_pressed() & BTN_ENTER) {
        sd_ok = sdcard_remount();
        watchdog_update();
        if (sd_ok)
          break;
      }
      sleep_ms(100);
    }
  }

  printf("SD card mounted OK\n");

  g_api.fs = &s_fs_impl;
  watchdog_update();

  watchdog_update();

  if (s_had_crash) {
    display_clear(COLOR_BLACK);
    display_draw_text(8, 8, "Recovered from crash", COLOR_YELLOW, COLOR_BLACK);
    display_draw_text(8, 24, "See /system/crashlog.txt", COLOR_GRAY, COLOR_BLACK);
    display_flush();
    watchdog_update();
    sleep_ms(2000);
    watchdog_update();
  }

  // Initialize the PSRAM allocator BEFORE anything that uses it
  // (SD card file ops use umm_malloc for FIL/FILINFO structs, config_load,
  // WiFi, Lua, OTA update, etc.)
  lua_psram_alloc_init();
  {
    void *pool = lua_psram_get_core1_pool();
    size_t pool_size = lua_psram_get_core1_pool_size();
    if (pool && pool_size > 0) {
      core1_alloc_init(pool, pool_size);
      // Flush dirty cache lines so Core 1 sees the init block header.
      // core1_alloc_init writes through Core 0's write-back XIP cache;
      // Core 1 has its own cache and would read stale zeros on a cold miss.
#ifndef PICOS_SIMULATOR
      __asm volatile ("dsb sy" ::: "memory");
      xip_cache_clean_all();
      __asm volatile ("isb sy" ::: "memory");
#endif
      printf("[MAIN] Core 1 allocator: %u KB at %p\n",
             (unsigned)(pool_size / 1024), pool);
    }
  }
  watchdog_update();

  // Check for pending OTA firmware update (must be before Core 1 launch).
  // Primary: watchdog scratch register set by ota_trigger_update().
  // Fallback: /system/update.bin exists on SD (manually placed firmware).
  bool ota_pending = ota_check_pending();
  if (!ota_pending && sdcard_fsize(OTA_BIN_PATH) > 0) {
    printf("[OTA] Found %s on SD — filesystem fallback trigger\n", OTA_BIN_PATH);
    ota_pending = true;
  }
  if (ota_pending) {
    ui_draw_splash("Applying firmware update...", "DO NOT POWER OFF!");
    watchdog_update();
    if (!ota_apply_update()) {
      // Update failed — show error briefly, continue normal boot
      display_clear(COLOR_BLACK);
      display_draw_text(8, 8, "Firmware update failed!", COLOR_RED, COLOR_BLACK);
      display_draw_text(8, 24, "Booting previous firmware.", COLOR_GRAY, COLOR_BLACK);
      display_flush();
      watchdog_update();
      sleep_ms(3000);
    }
    // ota_apply_update reboots on success, so we only get here on failure
  }
  watchdog_update();

  // Write crash log from previous boot (if any) — must be after PSRAM init
  // because sdcard_fopen() uses umm_malloc() for the FIL struct.
  crash_log_save();
  watchdog_update();

  // Load persisted settings from /system/config.json
  config_load();
  watchdog_update();

  // Initialise WiFi hardware (auto-connects if credentials are in config)
  ui_draw_splash("Initialising WiFi...", NULL);
  watchdog_update();
  toast_init();
  wifi_init();
  http_init();
  tcp_init();
  watchdog_update();

  // Debug: check free size after WiFi/HTTP init
  extern size_t lua_psram_alloc_free_size(void);
  printf("[PSRAM] Free after WiFi/HTTP init: %zu bytes (%zuK)\n",
         lua_psram_alloc_free_size(), lua_psram_alloc_free_size() / 1024);

  // Launch Core 1 background tasks
  multicore_launch_core1(core1_entry);
  watchdog_update();

  system_menu_init();

  ui_draw_splash("Loading...", NULL);
  sleep_ms(300); // Brief pause so the splash is visible

  // Boot completed successfully — clear the boot attempt counter so a
  // future watchdog reset starts fresh.
  watchdog_hw->scratch[0] = 0;

  // Hand off to the launcher — this never returns
  launcher_run();

  // Unreachable
  return 0;
}
