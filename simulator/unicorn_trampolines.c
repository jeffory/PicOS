// unicorn_trampolines.c — API dispatch layer for Unicorn Engine emulation.
// Each PicoCalcAPI function pointer in the emulated ARM address space points to
// a trampoline address (0xF0000000 + slot*4). A UC_HOOK_CODE on that region
// dispatches to the host implementation via this file.

#include <unicorn/unicorn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#include "os.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_sdcard.h"
#include "hal/hal_timing.h"

// From unicorn_runner.c
extern uc_engine *g_uc;
extern uint32_t handle_wrap(void *host_ptr);
extern void *handle_unwrap(uint32_t handle);
extern void handle_free(uint32_t handle);
extern uint32_t arena_write_string(uc_engine *uc, const char *str);
extern uint32_t arena_write_buffer(uc_engine *uc, const void *data, uint32_t len);
extern char *uc_read_string(uc_engine *uc, uint32_t addr);

// Memory map constants (must match unicorn_runner.c)
#define EMU_CODE_BASE       0x10000000u
#define EMU_FB_BASE         0xD0000000u
#define EMU_API_BASE        0xE0000000u
#define EMU_TRAMP_BASE      0xF0000000u
#define EMU_HEAP_BASE       0x40000000u
#define EMU_HEAP_SIZE       (4u * 1024 * 1024)

// From unicorn_runner.c
extern uint32_t heap_alloc(uint32_t size);
extern void heap_free(uint32_t addr);

// Heap functions made accessible (they were static in unicorn_runner.c,
// so we re-declare here and they will be moved to non-static later)
// Actually, let's just include them as extern and make them non-static in runner.

// Display functions (from simulator stubs)
extern void display_clear(uint16_t color);
extern void display_set_pixel(int x, int y, uint16_t color);
extern void display_fill_rect(int x, int y, int w, int h, uint16_t color);
extern void display_draw_rect(int x, int y, int w, int h, uint16_t color);
extern void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
extern void display_draw_circle(int cx, int cy, int r, uint16_t color);
extern void display_fill_circle(int cx, int cy, int r, uint16_t color);
extern int  display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
extern void display_flush(void);
extern void display_set_brightness(uint8_t brightness);
extern uint16_t *display_get_back_buffer(void);

// Input functions (from keyboard stub)
extern uint32_t kbd_get_buttons(void);
extern uint32_t kbd_get_buttons_pressed(void);
extern uint32_t kbd_get_buttons_released(void);
extern char kbd_get_char(void);

// Timing
extern uint32_t hal_get_time_ms(void);
extern uint64_t hal_get_time_us(void);

// System menu
extern void system_menu_add_item(const char *label, void (*cb)(void *user), void *user);
extern void system_menu_clear_items(void);

// FS functions (from stubs)
extern void *sdcard_fopen(const char *path, const char *mode);
extern int   sdcard_fread(void *f, void *buf, int len);
extern int   sdcard_fwrite(void *f, const void *buf, int len);
extern void  sdcard_fclose(void *f);
extern bool  sdcard_fexists(const char *path);
extern size_t sdcard_fsize(const char *path);
extern size_t sdcard_fsize_handle(void *f);
extern int   sdcard_fseek(void *f, long offset, int whence);
extern long  sdcard_ftell(void *f);

// Audio functions
extern void audio_play_tone(uint32_t freq, uint32_t dur);
extern void audio_stop_tone(void);
extern void audio_set_volume(uint8_t vol);
extern void audio_start_stream(uint32_t rate);
extern void audio_stop_stream(void);
extern void audio_push_samples(const int16_t *samples, int count);

// Perf functions
extern void perf_begin_frame(void);
extern void perf_end_frame(void);
extern int  perf_get_fps(void);
extern uint32_t perf_get_frame_time(void);
extern void perf_set_target_fps(uint32_t fps);

// WiFi functions (from sim_wifi.c)
extern void wifi_connect(const char *ssid, const char *pass);
extern void wifi_disconnect(void);
extern int  wifi_get_status(void);
extern const char *wifi_get_ip(void);
extern const char *wifi_get_ssid(void);
extern bool wifi_is_available(void);

// Native audio callback
extern void (*g_native_audio_callback)(void);

// Dev commands
extern bool dev_commands_wants_exit(void);
extern void dev_commands_clear_exit(void);

// Keyboard
extern void kbd_poll(void);
extern bool kbd_consume_menu_press(void);

// System menu for native apps
extern bool system_menu_show_for_native(void);

// =============================================================================
// Helper: read ARM register values
// =============================================================================

static uint32_t read_reg(uc_engine *uc, int reg) {
    uint32_t val;
    uc_reg_read(uc, reg, &val);
    return val;
}

static void write_reg(uc_engine *uc, int reg, uint32_t val) {
    uc_reg_write(uc, reg, &val);
}

static uint32_t read_stack_arg(uc_engine *uc, int index) {
    // Args beyond r0-r3 are on the stack: SP + index*4
    uint32_t sp = read_reg(uc, UC_ARM_REG_SP);
    uint32_t val;
    uc_mem_read(uc, sp + (uint32_t)index * 4, &val, 4);
    return val;
}

// =============================================================================
// Slot assignment — organized by sub-table
// =============================================================================

// Each sub-table's first slot. Slots within a sub-table are sequential.
// The offsets here define the struct layout in emulated memory.
enum {
    // picocalc_display_t (25 functions)
    SLOT_DISPLAY_CLEAR = 0,
    SLOT_DISPLAY_SET_PIXEL,
    SLOT_DISPLAY_FILL_RECT,
    SLOT_DISPLAY_DRAW_RECT,
    SLOT_DISPLAY_DRAW_LINE,
    SLOT_DISPLAY_DRAW_CIRCLE,
    SLOT_DISPLAY_FILL_CIRCLE,
    SLOT_DISPLAY_DRAW_TEXT,
    SLOT_DISPLAY_FLUSH,
    SLOT_DISPLAY_GET_WIDTH,
    SLOT_DISPLAY_GET_HEIGHT,
    SLOT_DISPLAY_SET_BRIGHTNESS,
    SLOT_DISPLAY_DRAW_IMAGE_NN,
    SLOT_DISPLAY_FLUSH_ROWS,
    SLOT_DISPLAY_FLUSH_REGION,
    SLOT_DISPLAY_GET_BACK_BUFFER,
    SLOT_DISPLAY_EFFECT_INVERT,
    SLOT_DISPLAY_EFFECT_DARKEN,
    SLOT_DISPLAY_EFFECT_BRIGHTEN,
    SLOT_DISPLAY_EFFECT_TINT,
    SLOT_DISPLAY_EFFECT_GRAYSCALE,
    SLOT_DISPLAY_EFFECT_BLEND,
    SLOT_DISPLAY_EFFECT_PALETTE,
    SLOT_DISPLAY_EFFECT_DITHER,
    SLOT_DISPLAY_EFFECT_SCANLINE,
    SLOT_DISPLAY_EFFECT_POSTERIZE,
    SLOT_DISPLAY_END,

    // picocalc_input_t (4 functions)
    SLOT_INPUT_GET_BUTTONS = SLOT_DISPLAY_END,
    SLOT_INPUT_GET_BUTTONS_PRESSED,
    SLOT_INPUT_GET_BUTTONS_RELEASED,
    SLOT_INPUT_GET_CHAR,
    SLOT_INPUT_END,

    // picocalc_fs_t (10 functions)
    SLOT_FS_OPEN = SLOT_INPUT_END,
    SLOT_FS_READ,
    SLOT_FS_WRITE,
    SLOT_FS_CLOSE,
    SLOT_FS_EXISTS,
    SLOT_FS_SIZE,
    SLOT_FS_FSIZE,
    SLOT_FS_SEEK,
    SLOT_FS_TELL,
    SLOT_FS_LIST_DIR,
    SLOT_FS_END,

    // picocalc_sys_t (11 functions)
    SLOT_SYS_GET_TIME_MS = SLOT_FS_END,
    SLOT_SYS_GET_TIME_US,
    SLOT_SYS_REBOOT,
    SLOT_SYS_GET_BATTERY,
    SLOT_SYS_IS_USB_POWERED,
    SLOT_SYS_ADD_MENU_ITEM,
    SLOT_SYS_CLEAR_MENU_ITEMS,
    SLOT_SYS_LOG,
    SLOT_SYS_POLL,
    SLOT_SYS_SHOULD_EXIT,
    SLOT_SYS_SET_AUDIO_CALLBACK,
    SLOT_SYS_END,

    // picocalc_audio_t (6 functions)
    SLOT_AUDIO_PLAY_TONE = SLOT_SYS_END,
    SLOT_AUDIO_STOP_TONE,
    SLOT_AUDIO_SET_VOLUME,
    SLOT_AUDIO_START_STREAM,
    SLOT_AUDIO_STOP_STREAM,
    SLOT_AUDIO_PUSH_SAMPLES,
    SLOT_AUDIO_END,

    // picocalc_wifi_t (6 functions)
    SLOT_WIFI_CONNECT = SLOT_AUDIO_END,
    SLOT_WIFI_DISCONNECT,
    SLOT_WIFI_GET_STATUS,
    SLOT_WIFI_GET_IP,
    SLOT_WIFI_GET_SSID,
    SLOT_WIFI_IS_AVAILABLE,
    SLOT_WIFI_END,

    // picocalc_tcp_t (7 functions) — stub
    SLOT_TCP_CONNECT = SLOT_WIFI_END,
    SLOT_TCP_WRITE,
    SLOT_TCP_READ,
    SLOT_TCP_CLOSE,
    SLOT_TCP_AVAILABLE,
    SLOT_TCP_GET_ERROR,
    SLOT_TCP_GET_EVENTS,
    SLOT_TCP_END,

    // picocalc_ui_t (3 functions) — stub
    SLOT_UI_TEXT_INPUT = SLOT_TCP_END,
    SLOT_UI_TEXT_INPUT_SIMPLE,
    SLOT_UI_CONFIRM,
    SLOT_UI_END,

    // picocalc_psram_t (8 functions)
    SLOT_PSRAM_PIO_AVAILABLE = SLOT_UI_END,
    SLOT_PSRAM_PIO_BULK_AVAILABLE,
    SLOT_PSRAM_PIO_READ,
    SLOT_PSRAM_PIO_WRITE,
    SLOT_PSRAM_PIO_BULK_READ,
    SLOT_PSRAM_PIO_BULK_WRITE,
    SLOT_PSRAM_QMI_ALLOC,
    SLOT_PSRAM_QMI_FREE,
    SLOT_PSRAM_END,

    // picocalc_perf_t (6 functions)
    SLOT_PERF_BEGIN_FRAME = SLOT_PSRAM_END,
    SLOT_PERF_END_FRAME,
    SLOT_PERF_GET_FPS,
    SLOT_PERF_GET_FRAME_TIME,
    SLOT_PERF_DRAW_FPS,
    SLOT_PERF_SET_TARGET_FPS,
    SLOT_PERF_END,

    // picocalc_terminal_t (47 functions) — all stubbed
    SLOT_TERMINAL_START = SLOT_PERF_END,
    SLOT_TERMINAL_END = SLOT_TERMINAL_START + 47,

    // picocalc_http_t (15 functions) — stub
    SLOT_HTTP_START = SLOT_TERMINAL_END,
    SLOT_HTTP_END = SLOT_HTTP_START + 15,

    // picocalc_soundplayer_t (35 functions) — stub
    SLOT_SOUNDPLAYER_START = SLOT_HTTP_END,
    SLOT_SOUNDPLAYER_END = SLOT_SOUNDPLAYER_START + 35,

    // picocalc_appconfig_t (7 functions) — stub
    SLOT_APPCONFIG_START = SLOT_SOUNDPLAYER_END,
    SLOT_APPCONFIG_END = SLOT_APPCONFIG_START + 7,

    // picocalc_crypto_t (16 functions) — stub
    SLOT_CRYPTO_START = SLOT_APPCONFIG_END,
    SLOT_CRYPTO_END = SLOT_CRYPTO_START + 16,

    // picocalc_graphics_t (10 functions) — stub
    SLOT_GRAPHICS_START = SLOT_CRYPTO_END,
    SLOT_GRAPHICS_END = SLOT_GRAPHICS_START + 10,

    // picocalc_video_t (22 functions) — stub
    SLOT_VIDEO_START = SLOT_GRAPHICS_END,
    SLOT_VIDEO_END = SLOT_VIDEO_START + 22,

    SLOT_TOTAL_COUNT = SLOT_VIDEO_END,
};

// =============================================================================
// Native exit flag (mirrors s_native_exit in src/main.c)
// =============================================================================
static volatile bool s_emu_exit = false;

// =============================================================================
// Display trampoline handlers
// =============================================================================

static void tramp_display_clear(uc_engine *uc) {
    uint16_t color = (uint16_t)read_reg(uc, UC_ARM_REG_R0);
    display_clear(color);
}

static void tramp_display_set_pixel(uc_engine *uc) {
    int x = (int)read_reg(uc, UC_ARM_REG_R0);
    int y = (int)read_reg(uc, UC_ARM_REG_R1);
    uint16_t color = (uint16_t)read_reg(uc, UC_ARM_REG_R2);
    display_set_pixel(x, y, color);
}

static void tramp_display_fill_rect(uc_engine *uc) {
    int x = (int)read_reg(uc, UC_ARM_REG_R0);
    int y = (int)read_reg(uc, UC_ARM_REG_R1);
    int w = (int)read_reg(uc, UC_ARM_REG_R2);
    int h = (int)read_reg(uc, UC_ARM_REG_R3);
    uint16_t color = (uint16_t)read_stack_arg(uc, 0);
    display_fill_rect(x, y, w, h, color);
}

static void tramp_display_draw_rect(uc_engine *uc) {
    int x = (int)read_reg(uc, UC_ARM_REG_R0);
    int y = (int)read_reg(uc, UC_ARM_REG_R1);
    int w = (int)read_reg(uc, UC_ARM_REG_R2);
    int h = (int)read_reg(uc, UC_ARM_REG_R3);
    uint16_t color = (uint16_t)read_stack_arg(uc, 0);
    display_draw_rect(x, y, w, h, color);
}

static void tramp_display_draw_line(uc_engine *uc) {
    int x0 = (int)read_reg(uc, UC_ARM_REG_R0);
    int y0 = (int)read_reg(uc, UC_ARM_REG_R1);
    int x1 = (int)read_reg(uc, UC_ARM_REG_R2);
    int y1 = (int)read_reg(uc, UC_ARM_REG_R3);
    uint16_t color = (uint16_t)read_stack_arg(uc, 0);
    display_draw_line(x0, y0, x1, y1, color);
}

static void tramp_display_draw_circle(uc_engine *uc) {
    int cx = (int)read_reg(uc, UC_ARM_REG_R0);
    int cy = (int)read_reg(uc, UC_ARM_REG_R1);
    int r  = (int)read_reg(uc, UC_ARM_REG_R2);
    uint16_t color = (uint16_t)read_reg(uc, UC_ARM_REG_R3);
    display_draw_circle(cx, cy, r, color);
}

static void tramp_display_fill_circle(uc_engine *uc) {
    int cx = (int)read_reg(uc, UC_ARM_REG_R0);
    int cy = (int)read_reg(uc, UC_ARM_REG_R1);
    int r  = (int)read_reg(uc, UC_ARM_REG_R2);
    uint16_t color = (uint16_t)read_reg(uc, UC_ARM_REG_R3);
    display_fill_circle(cx, cy, r, color);
}

static void tramp_display_draw_text(uc_engine *uc) {
    int x = (int)read_reg(uc, UC_ARM_REG_R0);
    int y = (int)read_reg(uc, UC_ARM_REG_R1);
    uint32_t text_addr = read_reg(uc, UC_ARM_REG_R2);
    uint16_t fg = (uint16_t)read_reg(uc, UC_ARM_REG_R3);
    uint16_t bg = (uint16_t)read_stack_arg(uc, 0);
    char *text = uc_read_string(uc, text_addr);
    int result = display_draw_text(x, y, text ? text : "", fg, bg);
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)result);
}

static void tramp_display_flush(uc_engine *uc) {
    // Copy the emulated framebuffer into the simulator's back buffer,
    // then present via the normal display_flush() path.
    uint16_t *back = display_get_back_buffer();
    uc_mem_read(uc, EMU_FB_BASE, back, 320 * 320 * sizeof(uint16_t));
    display_flush();
}

static void tramp_display_get_width(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, 320);
}

static void tramp_display_get_height(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, 320);
}

static void tramp_display_set_brightness(uc_engine *uc) {
    uint8_t brightness = (uint8_t)read_reg(uc, UC_ARM_REG_R0);
    display_set_brightness(brightness);
}

static void tramp_display_draw_image_nn(uc_engine *uc) {
    int x = (int)read_reg(uc, UC_ARM_REG_R0);
    int y = (int)read_reg(uc, UC_ARM_REG_R1);
    uint32_t data_addr = read_reg(uc, UC_ARM_REG_R2);
    int src_w = (int)read_reg(uc, UC_ARM_REG_R3);
    int src_h = (int)read_stack_arg(uc, 0);
    int scale = (int)read_stack_arg(uc, 1);

    // Read pixel data from emulated memory
    int pixels = src_w * src_h;
    if (pixels <= 0 || pixels > 320 * 320) return;
    uint16_t *buf = (uint16_t *)malloc(pixels * 2);
    if (!buf) return;
    uc_mem_read(uc, data_addr, buf, pixels * 2);

    // Use drawImage with nearest-neighbor scaling
    // The simulator doesn't have display_draw_image_nn, so implement it inline
    uint16_t *fb = display_get_back_buffer();
    for (int dy = 0; dy < src_h * scale && y + dy < 320; dy++) {
        for (int dx = 0; dx < src_w * scale && x + dx < 320; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && py >= 0) {
                fb[py * 320 + px] = buf[(dy / scale) * src_w + (dx / scale)];
            }
        }
    }
    free(buf);
}

static void tramp_display_flush_rows(uc_engine *uc) {
    // flushRows(y0, y1) — copy relevant rows from emulated FB, then flush
    uint32_t y0 = read_reg(uc, UC_ARM_REG_R0);
    uint32_t y1 = read_reg(uc, UC_ARM_REG_R1);
    if (y0 > 320) y0 = 0;
    if (y1 > 320) y1 = 320;
    uint16_t *back = display_get_back_buffer();
    uint32_t offset = y0 * 320 * sizeof(uint16_t);
    uint32_t len = (y1 - y0) * 320 * sizeof(uint16_t);
    uc_mem_read(uc, EMU_FB_BASE + offset, back + y0 * 320, len);
    display_flush();
}

static void tramp_display_flush_region(uc_engine *uc) {
    // flushRegion — copy entire emulated FB, then flush
    uint16_t *back = display_get_back_buffer();
    uc_mem_read(uc, EMU_FB_BASE, back, 320 * 320 * sizeof(uint16_t));
    display_flush();
}

static void tramp_display_get_back_buffer(uc_engine *uc) {
    // Return the emulated framebuffer address
    write_reg(uc, UC_ARM_REG_R0, EMU_FB_BASE);
}

static void tramp_display_effect_stub(uc_engine *uc) {
    (void)uc;
    // Effect stubs — most display effects are non-critical
}

// =============================================================================
// Input trampoline handlers
// =============================================================================

static void tramp_input_get_buttons(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, kbd_get_buttons());
}

static void tramp_input_get_buttons_pressed(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, kbd_get_buttons_pressed());
}

static void tramp_input_get_buttons_released(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, kbd_get_buttons_released());
}

static void tramp_input_get_char(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)(unsigned char)kbd_get_char());
}

// =============================================================================
// Filesystem trampoline handlers
// =============================================================================

static void tramp_fs_open(uc_engine *uc) {
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R0);
    uint32_t mode_addr = read_reg(uc, UC_ARM_REG_R1);
    char *path = uc_read_string(uc, path_addr);
    char *mode = uc_read_string(uc, mode_addr);
    void *f = sdcard_fopen(path ? path : "", mode ? mode : "r");
    uint32_t handle = handle_wrap(f);
    write_reg(uc, UC_ARM_REG_R0, handle);
}

static void tramp_fs_read(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t buf_addr = read_reg(uc, UC_ARM_REG_R1);
    int len = (int)read_reg(uc, UC_ARM_REG_R2);
    void *f = handle_unwrap(handle);
    if (!f || len <= 0) {
        write_reg(uc, UC_ARM_REG_R0, 0);
        return;
    }
    void *host_buf = malloc(len);
    if (!host_buf) {
        write_reg(uc, UC_ARM_REG_R0, 0);
        return;
    }
    int read_bytes = sdcard_fread(f, host_buf, len);
    if (read_bytes > 0) {
        uc_mem_write(uc, buf_addr, host_buf, read_bytes);
    }
    free(host_buf);
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)read_bytes);
}

static void tramp_fs_write(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t buf_addr = read_reg(uc, UC_ARM_REG_R1);
    int len = (int)read_reg(uc, UC_ARM_REG_R2);
    void *f = handle_unwrap(handle);
    if (!f || len <= 0) {
        write_reg(uc, UC_ARM_REG_R0, 0);
        return;
    }
    void *host_buf = malloc(len);
    if (!host_buf) {
        write_reg(uc, UC_ARM_REG_R0, 0);
        return;
    }
    uc_mem_read(uc, buf_addr, host_buf, len);
    int written = sdcard_fwrite(f, host_buf, len);
    free(host_buf);
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)written);
}

static void tramp_fs_close(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    void *f = handle_unwrap(handle);
    if (f) {
        sdcard_fclose(f);
        handle_free(handle);
    }
}

static void tramp_fs_exists(uc_engine *uc) {
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R0);
    char *path = uc_read_string(uc, path_addr);
    bool exists = sdcard_fexists(path ? path : "");
    write_reg(uc, UC_ARM_REG_R0, exists ? 1 : 0);
}

static void tramp_fs_size(uc_engine *uc) {
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R0);
    char *path = uc_read_string(uc, path_addr);
    int sz = (int)sdcard_fsize(path ? path : "");
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)sz);
}

static void tramp_fs_fsize(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    void *f = handle_unwrap(handle);
    int sz = f ? (int)sdcard_fsize_handle(f) : 0;
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)sz);
}

static void tramp_fs_seek(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t offset = read_reg(uc, UC_ARM_REG_R1);
    void *f = handle_unwrap(handle);
    bool ok = f ? (sdcard_fseek(f, (long)offset, 0) == 0) : false;
    write_reg(uc, UC_ARM_REG_R0, ok ? 1 : 0);
}

static void tramp_fs_tell(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    void *f = handle_unwrap(handle);
    uint32_t pos = f ? (uint32_t)sdcard_ftell(f) : 0;
    write_reg(uc, UC_ARM_REG_R0, pos);
}

// listDir callback context for nested emulation
typedef struct {
    uc_engine *uc;
    uint32_t arm_callback;
    uint32_t arm_user;
} listdir_ctx_t;

static void listdir_host_callback(const char *name, bool is_dir,
                                   uint32_t size, void *user) {
    // This is a simplified implementation. The full implementation would
    // need nested uc_emu_start(). For now, we don't support the callback.
    // The listDir in the emulated API uses a different callback signature
    // than sdcard_list_dir (which uses sdcard_entry_t*), so we need the
    // picocalc_fs_t callback signature: (name, is_dir, size, user).
    (void)name; (void)is_dir; (void)size; (void)user;
}

static void tramp_fs_list_dir(uc_engine *uc) {
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R0);
    uint32_t cb_addr   = read_reg(uc, UC_ARM_REG_R1);
    uint32_t user_addr = read_reg(uc, UC_ARM_REG_R2);
    char *path = uc_read_string(uc, path_addr);

    if (!path || cb_addr == 0) {
        write_reg(uc, UC_ARM_REG_R0, 0);
        return;
    }

    // We need to call sdcard_list_dir which uses sdcard_entry_t callback,
    // and for each entry, do a nested uc_emu_start to call the ARM callback.
    // For the initial implementation, collect entries then iterate.

    // Simple approach: use the host listDir and call back into ARM for each entry
    typedef struct {
        char name[256];
        bool is_dir;
        uint32_t size;
    } entry_info_t;

    #define MAX_ENTRIES 128
    static entry_info_t entries[MAX_ENTRIES];
    static int entry_count;
    entry_count = 0;

    // Collector callback
    typedef struct { int dummy; } sdcard_entry_t_local;

    // Use host filesystem directly (avoids sdcard_list_dir callback mismatch)
    char full_path[1024];
    extern char g_base_path[512];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", g_base_path, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", g_base_path, path);
    }

    // Use POSIX opendir/readdir
    DIR *dir = opendir(full_path);
    if (!dir) {
        write_reg(uc, UC_ARM_REG_R0, (uint32_t)-1);
        return;
    }

    int count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < MAX_ENTRIES) {
        if (de->d_name[0] == '.') continue;
        strncpy(entries[count].name, de->d_name, 255);
        entries[count].name[255] = '\0';

        char entry_path[1024];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, de->d_name);
        struct stat st;
        if (stat(entry_path, &st) == 0) {
            entries[count].is_dir = S_ISDIR(st.st_mode);
            entries[count].size = (uint32_t)st.st_size;
        } else {
            entries[count].is_dir = false;
            entries[count].size = 0;
        }
        count++;
    }
    closedir(dir);

    // Now call the ARM callback for each entry via nested emulation
    for (int i = 0; i < count; i++) {
        // Write the entry name string into the arena
        uint32_t name_addr = arena_write_string(uc, entries[i].name);

        // Save registers we'll clobber
        uint32_t save_r0, save_r1, save_r2, save_r3, save_lr, save_sp;
        uc_reg_read(uc, UC_ARM_REG_R0, &save_r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &save_r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &save_r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &save_r3);
        uc_reg_read(uc, UC_ARM_REG_LR, &save_lr);
        uc_reg_read(uc, UC_ARM_REG_SP, &save_sp);

        // Set up callback args: callback(name, is_dir, size, user)
        write_reg(uc, UC_ARM_REG_R0, name_addr);
        write_reg(uc, UC_ARM_REG_R1, entries[i].is_dir ? 1 : 0);
        write_reg(uc, UC_ARM_REG_R2, entries[i].size);
        write_reg(uc, UC_ARM_REG_R3, user_addr);

        // Set LR to 0 so the callback returns to address 0 (which stops emulation)
        uint32_t zero = 0;
        uc_reg_write(uc, UC_ARM_REG_LR, &zero);

        // Run nested emulation for the callback
        uint32_t callback_addr = cb_addr | 1u;  // Ensure Thumb bit
        uc_err err = uc_emu_start(uc, callback_addr, 0, 0, 0);
        if (err != UC_ERR_OK) {
            uint32_t pc;
            uc_reg_read(uc, UC_ARM_REG_PC, &pc);
            // PC=0 with FETCH_UNMAPPED means normal return
            if (!(err == UC_ERR_FETCH_UNMAPPED && pc == 0)) {
                fprintf(stderr, "[UNICORN] listDir callback error: %s (PC=0x%08x)\n",
                        uc_strerror(err), pc);
                break;
            }
        }

        // Restore registers
        uc_reg_write(uc, UC_ARM_REG_R0, &save_r0);
        uc_reg_write(uc, UC_ARM_REG_R1, &save_r1);
        uc_reg_write(uc, UC_ARM_REG_R2, &save_r2);
        uc_reg_write(uc, UC_ARM_REG_R3, &save_r3);
        uc_reg_write(uc, UC_ARM_REG_LR, &save_lr);
        uc_reg_write(uc, UC_ARM_REG_SP, &save_sp);
    }

    write_reg(uc, UC_ARM_REG_R0, (uint32_t)count);
    #undef MAX_ENTRIES
}

// =============================================================================
// System trampoline handlers
// =============================================================================

static void tramp_sys_get_time_ms(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, hal_get_time_ms());
}

static void tramp_sys_get_time_us(uc_engine *uc) {
    uint64_t us = hal_get_time_us();
    // ARM AAPCS: 64-bit return in r0 (low) and r1 (high)
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)(us & 0xFFFFFFFF));
    write_reg(uc, UC_ARM_REG_R1, (uint32_t)(us >> 32));
}

static void tramp_sys_reboot(uc_engine *uc) {
    (void)uc;
    printf("[UNICORN] App requested reboot — stopping emulation\n");
    uc_emu_stop(uc);
}

static void tramp_sys_get_battery(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, 100);  // Always 100% in simulator
}

static void tramp_sys_is_usb_powered(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, 1);  // Always USB in simulator
}

static void tramp_sys_add_menu_item(uc_engine *uc) {
    // addMenuItem(label, callback, user)
    // For now, just log it. Full implementation would need reverse callbacks.
    uint32_t label_addr = read_reg(uc, UC_ARM_REG_R0);
    char *label = uc_read_string(uc, label_addr);
    printf("[UNICORN] addMenuItem('%s') — stub\n", label ? label : "?");
}

static void tramp_sys_clear_menu_items(uc_engine *uc) {
    (void)uc;
    system_menu_clear_items();
}

static void tramp_sys_log(uc_engine *uc) {
    // Variadic log(fmt, ...) — parse format string and reconstruct
    uint32_t fmt_addr = read_reg(uc, UC_ARM_REG_R0);
    char *fmt = uc_read_string(uc, fmt_addr);
    if (!fmt) {
        printf("[APP] (null)\n");
        return;
    }

    // Collect up to 7 args from r1, r2, r3, then stack
    uint32_t args[7];
    args[0] = read_reg(uc, UC_ARM_REG_R1);
    args[1] = read_reg(uc, UC_ARM_REG_R2);
    args[2] = read_reg(uc, UC_ARM_REG_R3);
    for (int i = 3; i < 7; i++) {
        args[i] = read_stack_arg(uc, i - 3);
    }

    // Simple format string reconstruction
    // Walk the format string and substitute args
    char output[1024];
    int out_pos = 0;
    int arg_idx = 0;

    for (const char *p = fmt; *p && out_pos < 1020; p++) {
        if (*p != '%') {
            output[out_pos++] = *p;
            continue;
        }
        p++;
        if (!*p) break;

        // Skip flags, width, precision
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }

        // Handle length modifiers
        bool is_long = false;
        if (*p == 'l') { p++; is_long = true; if (*p == 'l') p++; }
        else if (*p == 'h') { p++; if (*p == 'h') p++; }

        if (arg_idx >= 7) {
            output[out_pos++] = '?';
            continue;
        }

        switch (*p) {
            case 'd': case 'i':
                out_pos += snprintf(output + out_pos, 1024 - out_pos, "%d", (int)args[arg_idx++]);
                break;
            case 'u':
                out_pos += snprintf(output + out_pos, 1024 - out_pos, "%u", args[arg_idx++]);
                break;
            case 'x':
                out_pos += snprintf(output + out_pos, 1024 - out_pos, "%x", args[arg_idx++]);
                break;
            case 'X':
                out_pos += snprintf(output + out_pos, 1024 - out_pos, "%X", args[arg_idx++]);
                break;
            case 'p':
                out_pos += snprintf(output + out_pos, 1024 - out_pos, "0x%x", args[arg_idx++]);
                break;
            case 'c':
                output[out_pos++] = (char)args[arg_idx++];
                break;
            case 's': {
                char *s = uc_read_string(uc, args[arg_idx++]);
                if (s) {
                    int slen = strlen(s);
                    if (out_pos + slen < 1020) {
                        memcpy(output + out_pos, s, slen);
                        out_pos += slen;
                    }
                }
                break;
            }
            case 'f': case 'g': case 'e': {
                // Float args are passed as doubles on ARM (8 bytes)
                // For simplicity, just print the raw uint32 value
                out_pos += snprintf(output + out_pos, 1024 - out_pos, "<float>");
                arg_idx++;
                break;
            }
            case '%':
                output[out_pos++] = '%';
                break;
            default:
                output[out_pos++] = '%';
                output[out_pos++] = *p;
                arg_idx++;
                break;
        }
    }
    output[out_pos] = '\0';
    printf("[APP] %s\n", output);
}

static void tramp_sys_poll(uc_engine *uc) {
    (void)uc;
    // Poll keyboard + check for exit
    kbd_poll();

    // Process socket commands (screenshot, exit, etc.) while emulation runs.
    // Without this, the socket handler is blocked by uc_emu_start().
    extern void sim_socket_poll(void);
    sim_socket_poll();

    if (kbd_consume_menu_press()) {
        if (system_menu_show_for_native()) {
            s_emu_exit = true;
        }
    }

    if (dev_commands_wants_exit()) {
        s_emu_exit = true;
        dev_commands_clear_exit();
    }

    // Yield CPU to avoid burning 100% in tight poll loops.
    // Real hardware's display_flush blocks ~65ms; emulated apps need throttling.
    struct timespec ts = {0, 1000000};  // 1ms
    nanosleep(&ts, NULL);
}

static void tramp_sys_should_exit(uc_engine *uc) {
    // Return the flag without clearing it — on real hardware, shouldExit()
    // stays true once set so the app sees it on every subsequent check.
    write_reg(uc, UC_ARM_REG_R0, s_emu_exit ? 1 : 0);
}

static void tramp_sys_set_audio_callback(uc_engine *uc) {
    // Can't easily support reverse callbacks for audio thread
    uint32_t cb_addr = read_reg(uc, UC_ARM_REG_R0);
    if (cb_addr) {
        printf("[UNICORN] setAudioCallback(0x%08x) — stub (not supported in emulation)\n", cb_addr);
    }
}

// =============================================================================
// Audio trampoline handlers
// =============================================================================

static void tramp_audio_play_tone(uc_engine *uc) {
    uint32_t freq = read_reg(uc, UC_ARM_REG_R0);
    uint32_t dur = read_reg(uc, UC_ARM_REG_R1);
    audio_play_tone(freq, dur);
}

static void tramp_audio_stop_tone(uc_engine *uc) {
    (void)uc;
    audio_stop_tone();
}

static void tramp_audio_set_volume(uc_engine *uc) {
    uint8_t vol = (uint8_t)read_reg(uc, UC_ARM_REG_R0);
    audio_set_volume(vol);
}

static void tramp_audio_start_stream(uc_engine *uc) {
    uint32_t rate = read_reg(uc, UC_ARM_REG_R0);
    audio_start_stream(rate);
}

static void tramp_audio_stop_stream(uc_engine *uc) {
    (void)uc;
    audio_stop_stream();
}

static void tramp_audio_push_samples(uc_engine *uc) {
    uint32_t samples_addr = read_reg(uc, UC_ARM_REG_R0);
    int count = (int)read_reg(uc, UC_ARM_REG_R1);
    if (count <= 0 || count > 16384) return;
    int16_t *buf = (int16_t *)malloc(count * 2 * sizeof(int16_t));
    if (!buf) return;
    uc_mem_read(uc, samples_addr, buf, count * 2 * sizeof(int16_t));
    audio_push_samples(buf, count);
    free(buf);
}

// =============================================================================
// WiFi trampoline handlers
// =============================================================================

static void tramp_wifi_connect(uc_engine *uc) {
    uint32_t ssid_addr = read_reg(uc, UC_ARM_REG_R0);
    uint32_t pass_addr = read_reg(uc, UC_ARM_REG_R1);
    char *ssid = uc_read_string(uc, ssid_addr);
    char *pass = uc_read_string(uc, pass_addr);
    wifi_connect(ssid ? ssid : "", pass ? pass : "");
}

static void tramp_wifi_disconnect(uc_engine *uc) {
    (void)uc;
    wifi_disconnect();
}

static void tramp_wifi_get_status(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)wifi_get_status());
}

static void tramp_wifi_get_ip(uc_engine *uc) {
    const char *ip = wifi_get_ip();
    uint32_t addr = arena_write_string(uc, ip);
    write_reg(uc, UC_ARM_REG_R0, addr);
}

static void tramp_wifi_get_ssid(uc_engine *uc) {
    const char *ssid = wifi_get_ssid();
    uint32_t addr = arena_write_string(uc, ssid);
    write_reg(uc, UC_ARM_REG_R0, addr);
}

static void tramp_wifi_is_available(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, wifi_is_available() ? 1 : 0);
}

// =============================================================================
// PSRAM trampoline handlers
// =============================================================================

static void tramp_psram_pio_available(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, 0);  // No PIO PSRAM in simulator
}

static void tramp_psram_pio_bulk_available(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, 0);
}

static void tramp_psram_qmi_alloc(uc_engine *uc) {
    uint32_t size = read_reg(uc, UC_ARM_REG_R0);
    uint32_t addr = heap_alloc(size);
    write_reg(uc, UC_ARM_REG_R0, addr);
}

static void tramp_psram_qmi_free(uc_engine *uc) {
    uint32_t addr = read_reg(uc, UC_ARM_REG_R0);
    heap_free(addr);
}

// =============================================================================
// Perf trampoline handlers
// =============================================================================

static void tramp_perf_begin_frame(uc_engine *uc) {
    (void)uc;
    perf_begin_frame();
}

static void tramp_perf_end_frame(uc_engine *uc) {
    (void)uc;
    perf_end_frame();
}

static void tramp_perf_get_fps(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)perf_get_fps());
}

static void tramp_perf_get_frame_time(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, perf_get_frame_time());
}

static void tramp_perf_draw_fps(uc_engine *uc) {
    int x = (int)read_reg(uc, UC_ARM_REG_R0);
    int y = (int)read_reg(uc, UC_ARM_REG_R1);
    // perf_draw_fps is a wrapper in main.c, not available in simulator
    // Draw FPS manually
    int fps = perf_get_fps();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d fps", fps);
    display_draw_text(x, y, buf, 0xFFFF, 0x0000);
}

static void tramp_perf_set_target_fps(uc_engine *uc) {
    uint32_t fps = read_reg(uc, UC_ARM_REG_R0);
    perf_set_target_fps(fps);
}

// =============================================================================
// Generic stub handler for unimplemented functions
// =============================================================================

static const char *s_stub_names[] = {
    [SLOT_TCP_CONNECT]      = "tcp->connect",
    [SLOT_TCP_WRITE]        = "tcp->write",
    [SLOT_TCP_READ]         = "tcp->read",
    [SLOT_TCP_CLOSE]        = "tcp->close",
    [SLOT_TCP_AVAILABLE]    = "tcp->available",
    [SLOT_TCP_GET_ERROR]    = "tcp->getError",
    [SLOT_TCP_GET_EVENTS]   = "tcp->getEvents",
    [SLOT_UI_TEXT_INPUT]    = "ui->textInput",
    [SLOT_UI_TEXT_INPUT_SIMPLE] = "ui->textInputSimple",
    [SLOT_UI_CONFIRM]       = "ui->confirm",
};

static void tramp_stub(uc_engine *uc, uint32_t slot) {
    if (slot < sizeof(s_stub_names)/sizeof(s_stub_names[0]) && s_stub_names[slot]) {
        printf("[UNICORN] STUB: %s() not implemented\n", s_stub_names[slot]);
    } else if (slot >= SLOT_TERMINAL_START && slot < SLOT_TERMINAL_END) {
        // Only log once to avoid spam
        static bool terminal_warned = false;
        if (!terminal_warned) {
            printf("[UNICORN] STUB: terminal function (slot %u) not implemented\n", slot);
            terminal_warned = true;
        }
    } else if (slot >= SLOT_HTTP_START && slot < SLOT_HTTP_END) {
        static bool http_warned = false;
        if (!http_warned) {
            printf("[UNICORN] STUB: http function (slot %u) not implemented\n", slot);
            http_warned = true;
        }
    } else if (slot >= SLOT_SOUNDPLAYER_START && slot < SLOT_SOUNDPLAYER_END) {
        static bool sound_warned = false;
        if (!sound_warned) {
            printf("[UNICORN] STUB: soundplayer function (slot %u) not implemented\n", slot);
            sound_warned = true;
        }
    } else if (slot >= SLOT_APPCONFIG_START && slot < SLOT_APPCONFIG_END) {
        static bool appconfig_warned = false;
        if (!appconfig_warned) {
            printf("[UNICORN] STUB: appconfig function (slot %u) not implemented\n", slot);
            appconfig_warned = true;
        }
    } else if (slot >= SLOT_CRYPTO_START && slot < SLOT_CRYPTO_END) {
        static bool crypto_warned = false;
        if (!crypto_warned) {
            printf("[UNICORN] STUB: crypto function (slot %u) not implemented\n", slot);
            crypto_warned = true;
        }
    } else if (slot >= SLOT_GRAPHICS_START && slot < SLOT_GRAPHICS_END) {
        static bool graphics_warned = false;
        if (!graphics_warned) {
            printf("[UNICORN] STUB: graphics function (slot %u) not implemented\n", slot);
            graphics_warned = true;
        }
    } else if (slot >= SLOT_VIDEO_START && slot < SLOT_VIDEO_END) {
        static bool video_warned = false;
        if (!video_warned) {
            printf("[UNICORN] STUB: video function (slot %u) not implemented\n", slot);
            video_warned = true;
        }
    } else {
        printf("[UNICORN] STUB: unknown slot %u called\n", slot);
    }
    write_reg(uc, UC_ARM_REG_R0, 0);
}

// =============================================================================
// Dispatch table
// =============================================================================

typedef void (*tramp_handler_t)(uc_engine *uc);

static tramp_handler_t s_dispatch[SLOT_TOTAL_COUNT];

void unicorn_tramp_init(uc_engine *uc) {
    (void)uc;
    memset(s_dispatch, 0, sizeof(s_dispatch));

    // Display
    s_dispatch[SLOT_DISPLAY_CLEAR]          = tramp_display_clear;
    s_dispatch[SLOT_DISPLAY_SET_PIXEL]      = tramp_display_set_pixel;
    s_dispatch[SLOT_DISPLAY_FILL_RECT]      = tramp_display_fill_rect;
    s_dispatch[SLOT_DISPLAY_DRAW_RECT]      = tramp_display_draw_rect;
    s_dispatch[SLOT_DISPLAY_DRAW_LINE]      = tramp_display_draw_line;
    s_dispatch[SLOT_DISPLAY_DRAW_CIRCLE]    = tramp_display_draw_circle;
    s_dispatch[SLOT_DISPLAY_FILL_CIRCLE]    = tramp_display_fill_circle;
    s_dispatch[SLOT_DISPLAY_DRAW_TEXT]      = tramp_display_draw_text;
    s_dispatch[SLOT_DISPLAY_FLUSH]          = tramp_display_flush;
    s_dispatch[SLOT_DISPLAY_GET_WIDTH]      = tramp_display_get_width;
    s_dispatch[SLOT_DISPLAY_GET_HEIGHT]     = tramp_display_get_height;
    s_dispatch[SLOT_DISPLAY_SET_BRIGHTNESS] = tramp_display_set_brightness;
    s_dispatch[SLOT_DISPLAY_DRAW_IMAGE_NN]  = tramp_display_draw_image_nn;
    s_dispatch[SLOT_DISPLAY_FLUSH_ROWS]     = tramp_display_flush_rows;
    s_dispatch[SLOT_DISPLAY_FLUSH_REGION]   = tramp_display_flush_region;
    s_dispatch[SLOT_DISPLAY_GET_BACK_BUFFER]= tramp_display_get_back_buffer;
    s_dispatch[SLOT_DISPLAY_EFFECT_INVERT]  = tramp_display_effect_stub;
    s_dispatch[SLOT_DISPLAY_EFFECT_DARKEN]  = tramp_display_effect_stub;
    s_dispatch[SLOT_DISPLAY_EFFECT_BRIGHTEN]= tramp_display_effect_stub;
    s_dispatch[SLOT_DISPLAY_EFFECT_TINT]    = tramp_display_effect_stub;
    s_dispatch[SLOT_DISPLAY_EFFECT_GRAYSCALE]= tramp_display_effect_stub;
    s_dispatch[SLOT_DISPLAY_EFFECT_BLEND]   = tramp_display_effect_stub;
    s_dispatch[SLOT_DISPLAY_EFFECT_PALETTE] = tramp_display_effect_stub;
    s_dispatch[SLOT_DISPLAY_EFFECT_DITHER]  = tramp_display_effect_stub;
    s_dispatch[SLOT_DISPLAY_EFFECT_SCANLINE]= tramp_display_effect_stub;
    s_dispatch[SLOT_DISPLAY_EFFECT_POSTERIZE]= tramp_display_effect_stub;

    // Input
    s_dispatch[SLOT_INPUT_GET_BUTTONS]         = tramp_input_get_buttons;
    s_dispatch[SLOT_INPUT_GET_BUTTONS_PRESSED] = tramp_input_get_buttons_pressed;
    s_dispatch[SLOT_INPUT_GET_BUTTONS_RELEASED]= tramp_input_get_buttons_released;
    s_dispatch[SLOT_INPUT_GET_CHAR]            = tramp_input_get_char;

    // Filesystem
    s_dispatch[SLOT_FS_OPEN]      = tramp_fs_open;
    s_dispatch[SLOT_FS_READ]      = tramp_fs_read;
    s_dispatch[SLOT_FS_WRITE]     = tramp_fs_write;
    s_dispatch[SLOT_FS_CLOSE]     = tramp_fs_close;
    s_dispatch[SLOT_FS_EXISTS]    = tramp_fs_exists;
    s_dispatch[SLOT_FS_SIZE]      = tramp_fs_size;
    s_dispatch[SLOT_FS_FSIZE]     = tramp_fs_fsize;
    s_dispatch[SLOT_FS_SEEK]      = tramp_fs_seek;
    s_dispatch[SLOT_FS_TELL]      = tramp_fs_tell;
    s_dispatch[SLOT_FS_LIST_DIR]  = tramp_fs_list_dir;

    // System
    s_dispatch[SLOT_SYS_GET_TIME_MS]       = tramp_sys_get_time_ms;
    s_dispatch[SLOT_SYS_GET_TIME_US]       = tramp_sys_get_time_us;
    s_dispatch[SLOT_SYS_REBOOT]            = tramp_sys_reboot;
    s_dispatch[SLOT_SYS_GET_BATTERY]       = tramp_sys_get_battery;
    s_dispatch[SLOT_SYS_IS_USB_POWERED]    = tramp_sys_is_usb_powered;
    s_dispatch[SLOT_SYS_ADD_MENU_ITEM]     = tramp_sys_add_menu_item;
    s_dispatch[SLOT_SYS_CLEAR_MENU_ITEMS]  = tramp_sys_clear_menu_items;
    s_dispatch[SLOT_SYS_LOG]               = tramp_sys_log;
    s_dispatch[SLOT_SYS_POLL]              = tramp_sys_poll;
    s_dispatch[SLOT_SYS_SHOULD_EXIT]       = tramp_sys_should_exit;
    s_dispatch[SLOT_SYS_SET_AUDIO_CALLBACK]= tramp_sys_set_audio_callback;

    // Audio
    s_dispatch[SLOT_AUDIO_PLAY_TONE]    = tramp_audio_play_tone;
    s_dispatch[SLOT_AUDIO_STOP_TONE]    = tramp_audio_stop_tone;
    s_dispatch[SLOT_AUDIO_SET_VOLUME]   = tramp_audio_set_volume;
    s_dispatch[SLOT_AUDIO_START_STREAM] = tramp_audio_start_stream;
    s_dispatch[SLOT_AUDIO_STOP_STREAM]  = tramp_audio_stop_stream;
    s_dispatch[SLOT_AUDIO_PUSH_SAMPLES] = tramp_audio_push_samples;

    // WiFi
    s_dispatch[SLOT_WIFI_CONNECT]    = tramp_wifi_connect;
    s_dispatch[SLOT_WIFI_DISCONNECT] = tramp_wifi_disconnect;
    s_dispatch[SLOT_WIFI_GET_STATUS] = tramp_wifi_get_status;
    s_dispatch[SLOT_WIFI_GET_IP]     = tramp_wifi_get_ip;
    s_dispatch[SLOT_WIFI_GET_SSID]   = tramp_wifi_get_ssid;
    s_dispatch[SLOT_WIFI_IS_AVAILABLE]= tramp_wifi_is_available;

    // PSRAM
    s_dispatch[SLOT_PSRAM_PIO_AVAILABLE]      = tramp_psram_pio_available;
    s_dispatch[SLOT_PSRAM_PIO_BULK_AVAILABLE] = tramp_psram_pio_bulk_available;
    s_dispatch[SLOT_PSRAM_QMI_ALLOC]          = tramp_psram_qmi_alloc;
    s_dispatch[SLOT_PSRAM_QMI_FREE]           = tramp_psram_qmi_free;

    // Perf
    s_dispatch[SLOT_PERF_BEGIN_FRAME]   = tramp_perf_begin_frame;
    s_dispatch[SLOT_PERF_END_FRAME]     = tramp_perf_end_frame;
    s_dispatch[SLOT_PERF_GET_FPS]       = tramp_perf_get_fps;
    s_dispatch[SLOT_PERF_GET_FRAME_TIME]= tramp_perf_get_frame_time;
    s_dispatch[SLOT_PERF_DRAW_FPS]      = tramp_perf_draw_fps;
    s_dispatch[SLOT_PERF_SET_TARGET_FPS]= tramp_perf_set_target_fps;

    // All other slots (terminal, http, soundplayer, appconfig, crypto,
    // graphics, video) are left NULL and will be handled by the stub handler
}

void unicorn_tramp_dispatch(uc_engine *uc, uint32_t slot) {
    if (slot >= SLOT_TOTAL_COUNT) {
        printf("[UNICORN] Invalid trampoline slot: %u\n", slot);
        write_reg(uc, UC_ARM_REG_R0, 0);
        return;
    }
    if (s_dispatch[slot]) {
        s_dispatch[slot](uc);
    } else {
        tramp_stub(uc, slot);
    }
}

uint32_t unicorn_tramp_get_slot_count(void) {
    return SLOT_TOTAL_COUNT;
}

// =============================================================================
// Build the emulated PicoCalcAPI struct in Unicorn memory
// =============================================================================

// Helper: write a 32-bit value to emulated memory
static void write32(uc_engine *uc, uint32_t addr, uint32_t val) {
    uc_mem_write(uc, addr, &val, 4);
}

// Helper: write a function pointer table (array of trampoline addresses)
static uint32_t write_func_table(uc_engine *uc, uint32_t base_addr,
                                  uint32_t tramp_base, uint32_t first_slot,
                                  uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        // Trampoline address with Thumb bit set
        uint32_t tramp_addr = (tramp_base + (first_slot + i) * 4) | 1u;
        write32(uc, base_addr + i * 4, tramp_addr);
    }
    return base_addr + count * 4;
}

void unicorn_build_api_struct(uc_engine *uc, uint32_t api_base, uint32_t tramp_base) {
    // Layout: PicoCalcAPI struct at api_base, followed by sub-tables
    // PicoCalcAPI has 17 pointer fields + 1 uint32_t (version)
    uint32_t api_struct_size = 18 * 4;  // 17 pointers + version

    // Sub-tables start after the main struct
    uint32_t sub_base = api_base + api_struct_size;
    // Align to 4 bytes
    sub_base = (sub_base + 3) & ~3u;

    // --- Write sub-tables ---

    // picocalc_input_t (4 function pointers)
    uint32_t input_addr = sub_base;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_INPUT_GET_BUTTONS, 4);

    // picocalc_display_t (26 function pointers — count from enum)
    uint32_t display_addr = sub_base;
    uint32_t display_count = SLOT_DISPLAY_END - SLOT_DISPLAY_CLEAR;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_DISPLAY_CLEAR, display_count);

    // picocalc_fs_t (10 function pointers)
    uint32_t fs_addr = sub_base;
    uint32_t fs_count = SLOT_FS_END - SLOT_FS_OPEN;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_FS_OPEN, fs_count);

    // picocalc_sys_t (11 function pointers)
    uint32_t sys_addr = sub_base;
    uint32_t sys_count = SLOT_SYS_END - SLOT_SYS_GET_TIME_MS;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_SYS_GET_TIME_MS, sys_count);

    // picocalc_audio_t (6 function pointers)
    uint32_t audio_addr = sub_base;
    uint32_t audio_count = SLOT_AUDIO_END - SLOT_AUDIO_PLAY_TONE;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_AUDIO_PLAY_TONE, audio_count);

    // picocalc_wifi_t (6 function pointers)
    uint32_t wifi_addr = sub_base;
    uint32_t wifi_count = SLOT_WIFI_END - SLOT_WIFI_CONNECT;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_WIFI_CONNECT, wifi_count);

    // picocalc_tcp_t (7 function pointers)
    uint32_t tcp_addr = sub_base;
    uint32_t tcp_count = SLOT_TCP_END - SLOT_TCP_CONNECT;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_TCP_CONNECT, tcp_count);

    // picocalc_ui_t (3 function pointers)
    uint32_t ui_addr = sub_base;
    uint32_t ui_count = SLOT_UI_END - SLOT_UI_TEXT_INPUT;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_UI_TEXT_INPUT, ui_count);

    // picocalc_psram_t (8 function pointers)
    uint32_t psram_addr = sub_base;
    uint32_t psram_count = SLOT_PSRAM_END - SLOT_PSRAM_PIO_AVAILABLE;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_PSRAM_PIO_AVAILABLE, psram_count);

    // picocalc_perf_t (6 function pointers)
    uint32_t perf_addr = sub_base;
    uint32_t perf_count = SLOT_PERF_END - SLOT_PERF_BEGIN_FRAME;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_PERF_BEGIN_FRAME, perf_count);

    // picocalc_terminal_t (47 function pointers)
    uint32_t terminal_addr = sub_base;
    uint32_t terminal_count = SLOT_TERMINAL_END - SLOT_TERMINAL_START;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_TERMINAL_START, terminal_count);

    // picocalc_http_t (15 function pointers)
    uint32_t http_addr = sub_base;
    uint32_t http_count = SLOT_HTTP_END - SLOT_HTTP_START;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_HTTP_START, http_count);

    // picocalc_soundplayer_t (35 function pointers)
    uint32_t soundplayer_addr = sub_base;
    uint32_t soundplayer_count = SLOT_SOUNDPLAYER_END - SLOT_SOUNDPLAYER_START;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_SOUNDPLAYER_START, soundplayer_count);

    // picocalc_appconfig_t (7 function pointers)
    uint32_t appconfig_addr = sub_base;
    uint32_t appconfig_count = SLOT_APPCONFIG_END - SLOT_APPCONFIG_START;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_APPCONFIG_START, appconfig_count);

    // picocalc_crypto_t (16 function pointers)
    uint32_t crypto_addr = sub_base;
    uint32_t crypto_count = SLOT_CRYPTO_END - SLOT_CRYPTO_START;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_CRYPTO_START, crypto_count);

    // picocalc_graphics_t (10 function pointers)
    uint32_t graphics_addr = sub_base;
    uint32_t graphics_count = SLOT_GRAPHICS_END - SLOT_GRAPHICS_START;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_GRAPHICS_START, graphics_count);

    // picocalc_video_t (22 function pointers)
    uint32_t video_addr = sub_base;
    uint32_t video_count = SLOT_VIDEO_END - SLOT_VIDEO_START;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_VIDEO_START, video_count);

    printf("[UNICORN] API sub-tables written, total %u bytes at 0x%08x..0x%08x\n",
           sub_base - api_base, api_base, sub_base);
    printf("[UNICORN] Total trampoline slots: %u\n", SLOT_TOTAL_COUNT);

    // --- Write the main PicoCalcAPI struct ---
    // struct PicoCalcAPI {
    //   const picocalc_input_t   *input;       // offset 0
    //   const picocalc_display_t *display;      // offset 4
    //   const picocalc_fs_t      *fs;           // offset 8
    //   const picocalc_sys_t     *sys;          // offset 12
    //   const picocalc_audio_t   *audio;        // offset 16
    //   const picocalc_wifi_t    *wifi;         // offset 20
    //   const picocalc_tcp_t     *tcp;          // offset 24
    //   const picocalc_ui_t      *ui;           // offset 28
    //   const picocalc_psram_t   *psram;        // offset 32
    //   const picocalc_perf_t    *perf;         // offset 36
    //   const picocalc_terminal_t *terminal;    // offset 40
    //   const picocalc_http_t    *http;         // offset 44
    //   const picocalc_soundplayer_t *soundplayer; // offset 48
    //   const picocalc_appconfig_t *appconfig;  // offset 52
    //   const picocalc_crypto_t  *crypto;       // offset 56
    //   const picocalc_graphics_t *graphics;    // offset 60
    //   const picocalc_video_t   *video;        // offset 64
    //   uint32_t                  version;      // offset 68
    // };

    write32(uc, api_base +  0, input_addr);
    write32(uc, api_base +  4, display_addr);
    write32(uc, api_base +  8, fs_addr);
    write32(uc, api_base + 12, sys_addr);
    write32(uc, api_base + 16, audio_addr);
    write32(uc, api_base + 20, wifi_addr);
    write32(uc, api_base + 24, tcp_addr);
    write32(uc, api_base + 28, ui_addr);
    write32(uc, api_base + 32, psram_addr);
    write32(uc, api_base + 36, perf_addr);
    write32(uc, api_base + 40, terminal_addr);
    write32(uc, api_base + 44, http_addr);
    write32(uc, api_base + 48, soundplayer_addr);
    write32(uc, api_base + 52, appconfig_addr);
    write32(uc, api_base + 56, crypto_addr);
    write32(uc, api_base + 60, graphics_addr);
    write32(uc, api_base + 64, video_addr);
    write32(uc, api_base + 68, 2);  // version = 2 (Phase 2)

    printf("[UNICORN] PicoCalcAPI struct at 0x%08x, version=2\n", api_base);
}
