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
#include "http.h"
#include "tcp.h"
#include "sound.h"
#include "fileplayer.h"
#include "mp3_player.h"
#include "terminal.h"
#include "appconfig.h"
#include "image_api.h"

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

// HTTP (from sim_http.c)
extern http_conn_t *http_alloc(void);
extern void http_free(http_conn_t *c);
extern void http_close(http_conn_t *c);
extern bool http_get(http_conn_t *c, const char *path, const char *extra_hdr);
extern bool http_post(http_conn_t *c, const char *path, const char *extra_hdr,
                      const char *body, size_t body_len);
extern uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len);
extern uint32_t http_bytes_available(http_conn_t *c);
extern bool http_set_recv_buf(http_conn_t *c, uint32_t bytes);
extern http_conn_t *http_get_conn(int idx);

// TCP (from sim_tcp.c)
extern tcp_conn_t *tcp_alloc(void);
extern bool tcp_connect(tcp_conn_t *c, const char *host, uint16_t port, bool use_ssl);
extern int  tcp_write(tcp_conn_t *c, const void *buf, int len);
extern int  tcp_read(tcp_conn_t *c, void *buf, int len);
extern void tcp_close(tcp_conn_t *c);
extern uint32_t tcp_bytes_available(tcp_conn_t *c);
extern const char *tcp_get_error(tcp_conn_t *c);
extern uint32_t tcp_take_pending(tcp_conn_t *c);
extern void tcp_free(tcp_conn_t *c);

// Appconfig (from src/os/appconfig.c)
extern bool appconfig_load(const char *app_id);
extern bool appconfig_save(void);
extern const char *appconfig_get(const char *key, const char *fallback);
extern void appconfig_set(const char *key, const char *value);
extern void appconfig_clear(void);
extern bool appconfig_reset(void);
extern const char *appconfig_get_app_id(void);

// Sound (from sim_audio.c)
extern sound_sample_t *sound_sample_create(void);
extern void sound_sample_destroy(sound_sample_t *sample);
extern bool sound_sample_load(sound_sample_t *sample, const char *path);
extern sound_player_t *sound_player_create(void);
extern void sound_player_destroy(sound_player_t *player);
extern bool sound_player_set_sample(sound_player_t *player, sound_sample_t *sample);
extern void sound_player_play(sound_player_t *player, uint8_t repeat_count);
extern void sound_player_stop(sound_player_t *player);
extern bool sound_player_is_playing(const sound_player_t *player);
extern uint8_t sound_player_get_volume(const sound_player_t *player);
extern void sound_player_set_volume(sound_player_t *player, uint8_t volume);

// Fileplayer (from sim_audio.c)
extern fileplayer_t *fileplayer_create(void);
extern void fileplayer_destroy(fileplayer_t *player);
extern bool fileplayer_load(fileplayer_t *player, const char *path);
extern bool fileplayer_play(fileplayer_t *player, uint8_t repeat_count);
extern void fileplayer_stop(fileplayer_t *player);
extern void fileplayer_pause(fileplayer_t *player);
extern void fileplayer_resume(fileplayer_t *player);
extern bool fileplayer_is_playing(const fileplayer_t *player);
extern void fileplayer_set_volume(fileplayer_t *player, uint8_t left, uint8_t right);
extern uint32_t fileplayer_get_offset(const fileplayer_t *player);
extern void fileplayer_set_offset(fileplayer_t *player, uint32_t seconds);
extern bool fileplayer_did_underrun(void);

// MP3 player (from sim_audio.c)
extern mp3_player_t *mp3_player_create(void);
extern void mp3_player_destroy(mp3_player_t *player);
extern bool mp3_player_load(mp3_player_t *player, const char *path);
extern bool mp3_player_play(mp3_player_t *player, uint8_t repeat_count);
extern void mp3_player_stop(mp3_player_t *player);
extern void mp3_player_pause(mp3_player_t *player);
extern void mp3_player_resume(mp3_player_t *player);
extern bool mp3_player_is_playing(const mp3_player_t *player);
extern void mp3_player_set_volume(mp3_player_t *player, uint8_t volume);
extern uint8_t mp3_player_get_volume(const mp3_player_t *player);
extern void mp3_player_set_loop(mp3_player_t *player, bool loop);

// Terminal (from src/os/terminal.c)
extern terminal_t *terminal_new(int cols, int rows, int scrollback_lines);
extern void terminal_free(terminal_t *term);
extern void terminal_clear(terminal_t *term);
extern void terminal_putString(terminal_t *term, const char *s);
extern void terminal_putChar(terminal_t *term, char c);
extern void terminal_setCursor(terminal_t *term, int x, int y);
extern void terminal_getCursor(terminal_t *term, int *out_x, int *out_y);
extern void terminal_setColors(terminal_t *term, uint16_t fg, uint16_t bg);
extern void terminal_getColors(terminal_t *term, uint16_t *out_fg, uint16_t *out_bg);
extern void terminal_scroll(terminal_t *term, int lines);
extern void terminal_render(terminal_t *term);
extern void terminal_renderDirty(terminal_t *term);
extern int  terminal_getCols(terminal_t *term);
extern int  terminal_getRows(terminal_t *term);
extern void terminal_markAllDirty(terminal_t *term);
extern bool terminal_isFullDirty(terminal_t *term);
extern void terminal_getDirtyRange(terminal_t *term, int *out_first, int *out_last);
extern int  terminal_getScrollbackCount(terminal_t *term);
extern void terminal_setScrollbackOffset(terminal_t *term, int offset);
extern int  terminal_getScrollbackOffset(terminal_t *term);
extern void terminal_getScrollbackLine(terminal_t *term, int line, uint16_t *out_cells);
extern void terminal_getScrollbackLineColors(terminal_t *term, int line, uint16_t *out_fg, uint16_t *out_bg);
extern void terminal_setLineNumbers(terminal_t *term, bool enabled);
extern void terminal_setLineNumberStart(terminal_t *term, int start);
extern void terminal_setLineNumberCols(terminal_t *term, int cols);
extern void terminal_setLineNumberColors(terminal_t *term, uint16_t fg, uint16_t bg);
extern int  terminal_getContentCols(terminal_t *term);
extern void terminal_setScrollbar(terminal_t *term, bool enabled);
extern void terminal_setScrollbarColors(terminal_t *term, uint16_t bg, uint16_t thumb);
extern void terminal_setScrollbarWidth(terminal_t *term, int width);
extern void terminal_setScrollInfo(terminal_t *term, int total_lines, int scroll_position);
extern void terminal_setRenderBounds(terminal_t *term, int y_start, int y_end);
extern void terminal_setWordWrap(terminal_t *term, bool enabled);
extern void terminal_setWordWrapColumn(terminal_t *term, int column);
extern void terminal_setWrapIndicator(terminal_t *term, bool enabled);
extern bool terminal_getWordWrap(terminal_t *term);
extern int  terminal_getVisualRowCount(terminal_t *term);
extern void terminal_logicalToVisual(terminal_t *term, int log_x, int log_y, int *vis_x, int *vis_y);
extern void terminal_visualToLogical(terminal_t *term, int vis_x, int vis_y, int *log_x, int *log_y);
extern int  terminal_calculateLineWraps(terminal_t *term, int logical_line, int *segments, int max_segments);

// Video player (from stubs)
extern void *video_player_create(void);
extern void video_player_destroy(void *player);
extern int  video_player_load(void *player, const char *path);
extern void video_player_play(void *player);
extern void video_player_pause(void *player);
extern void video_player_resume(void *player);
extern void video_player_stop(void *player);
extern void video_player_update(void *player);
extern void video_player_seek(void *player, float pos);
extern float video_player_get_fps(void *player);
extern int  video_player_get_dropped_frames(void *player);
extern void video_player_reset_stats(void *player);
extern bool video_player_has_audio(void *player);
extern void video_player_set_audio_volume(void *player, uint8_t volume);
extern uint8_t video_player_get_audio_volume(void *player);
extern void video_player_set_audio_muted(void *player, bool muted);
extern bool video_player_get_audio_muted(void *player);

// Image (from stubs)
extern pc_image_t *image_load(const char *path);
extern pc_image_t *image_new_blank(int width, int height);

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

    // picocalc_terminal_t (47 functions)
    SLOT_TERM_CREATE = SLOT_PERF_END,
    SLOT_TERM_FREE,
    SLOT_TERM_CLEAR,
    SLOT_TERM_WRITE,
    SLOT_TERM_PUT_CHAR,
    SLOT_TERM_SET_CURSOR,
    SLOT_TERM_GET_CURSOR,
    SLOT_TERM_SET_COLORS,
    SLOT_TERM_GET_COLORS,
    SLOT_TERM_SCROLL,
    SLOT_TERM_RENDER,
    SLOT_TERM_RENDER_DIRTY,
    SLOT_TERM_GET_COLS,
    SLOT_TERM_GET_ROWS,
    SLOT_TERM_SET_CURSOR_VISIBLE,
    SLOT_TERM_SET_CURSOR_BLINK,
    SLOT_TERM_MARK_ALL_DIRTY,
    SLOT_TERM_IS_FULL_DIRTY,
    SLOT_TERM_GET_DIRTY_RANGE,
    SLOT_TERM_GET_SCROLLBACK_COUNT,
    SLOT_TERM_SET_SCROLLBACK_OFFSET,
    SLOT_TERM_GET_SCROLLBACK_OFFSET,
    SLOT_TERM_GET_SCROLLBACK_LINE,
    SLOT_TERM_GET_SCROLLBACK_LINE_COLORS,
    SLOT_TERM_SET_LINE_NUMBERS,
    SLOT_TERM_SET_LINE_NUMBER_START,
    SLOT_TERM_SET_LINE_NUMBER_COLS,
    SLOT_TERM_SET_LINE_NUMBER_COLORS,
    SLOT_TERM_GET_CONTENT_COLS,
    SLOT_TERM_SET_SCROLLBAR,
    SLOT_TERM_SET_SCROLLBAR_COLORS,
    SLOT_TERM_SET_SCROLLBAR_WIDTH,
    SLOT_TERM_SET_SCROLL_INFO,
    SLOT_TERM_SET_RENDER_BOUNDS,
    SLOT_TERM_SET_WORD_WRAP,
    SLOT_TERM_SET_WORD_WRAP_COLUMN,
    SLOT_TERM_SET_WRAP_INDICATOR,
    SLOT_TERM_GET_WORD_WRAP,
    SLOT_TERM_GET_VISUAL_ROW_COUNT,
    SLOT_TERM_LOGICAL_TO_VISUAL,
    SLOT_TERM_VISUAL_TO_LOGICAL,
    SLOT_TERM_CALCULATE_LINE_WRAPS,
    // 5 padding slots to maintain compatibility with os.h struct size
    SLOT_TERM_PAD1, SLOT_TERM_PAD2, SLOT_TERM_PAD3, SLOT_TERM_PAD4, SLOT_TERM_PAD5,
    SLOT_TERMINAL_END,

    // picocalc_http_t (15 functions)
    SLOT_HTTP_NEW_CONN = SLOT_TERMINAL_END,
    SLOT_HTTP_GET,
    SLOT_HTTP_POST,
    SLOT_HTTP_READ,
    SLOT_HTTP_AVAILABLE,
    SLOT_HTTP_CLOSE,
    SLOT_HTTP_GET_STATUS,
    SLOT_HTTP_GET_ERROR,
    SLOT_HTTP_GET_PROGRESS,
    SLOT_HTTP_SET_KEEP_ALIVE,
    SLOT_HTTP_SET_BYTE_RANGE,
    SLOT_HTTP_SET_CONNECT_TIMEOUT,
    SLOT_HTTP_SET_READ_TIMEOUT,
    SLOT_HTTP_SET_READ_BUFFER_SIZE,
    // 1 padding slot
    SLOT_HTTP_PAD1,
    SLOT_HTTP_END,

    // picocalc_soundplayer_t (35 functions)
    SLOT_SND_SAMPLE_LOAD = SLOT_HTTP_END,
    SLOT_SND_SAMPLE_FREE,
    SLOT_SND_PLAYER_NEW,
    SLOT_SND_PLAYER_SET_SAMPLE,
    SLOT_SND_PLAYER_PLAY,
    SLOT_SND_PLAYER_STOP,
    SLOT_SND_PLAYER_IS_PLAYING,
    SLOT_SND_PLAYER_GET_VOLUME,
    SLOT_SND_PLAYER_SET_VOLUME,
    SLOT_SND_PLAYER_SET_LOOP,
    SLOT_SND_PLAYER_FREE,
    SLOT_SND_FP_NEW,
    SLOT_SND_FP_LOAD,
    SLOT_SND_FP_PLAY,
    SLOT_SND_FP_STOP,
    SLOT_SND_FP_PAUSE,
    SLOT_SND_FP_RESUME,
    SLOT_SND_FP_IS_PLAYING,
    SLOT_SND_FP_SET_VOLUME,
    SLOT_SND_FP_GET_VOLUME,
    SLOT_SND_FP_GET_OFFSET,
    SLOT_SND_FP_SET_OFFSET,
    SLOT_SND_FP_DID_UNDERRUN,
    SLOT_SND_FP_FREE,
    SLOT_SND_MP3_NEW,
    SLOT_SND_MP3_LOAD,
    SLOT_SND_MP3_PLAY,
    SLOT_SND_MP3_STOP,
    SLOT_SND_MP3_PAUSE,
    SLOT_SND_MP3_RESUME,
    SLOT_SND_MP3_IS_PLAYING,
    SLOT_SND_MP3_SET_VOLUME,
    SLOT_SND_MP3_GET_VOLUME,
    SLOT_SND_MP3_SET_LOOP,
    SLOT_SND_MP3_FREE,
    SLOT_SOUNDPLAYER_END,

    // picocalc_appconfig_t (7 functions)
    SLOT_APPCONFIG_LOAD = SLOT_SOUNDPLAYER_END,
    SLOT_APPCONFIG_SAVE,
    SLOT_APPCONFIG_GET,
    SLOT_APPCONFIG_SET,
    SLOT_APPCONFIG_CLEAR,
    SLOT_APPCONFIG_RESET,
    SLOT_APPCONFIG_GET_APP_ID,
    SLOT_APPCONFIG_END,

    // picocalc_crypto_t (16 functions)
    SLOT_CRYPTO_SHA256 = SLOT_APPCONFIG_END,
    SLOT_CRYPTO_SHA1,
    SLOT_CRYPTO_HMAC_SHA256,
    SLOT_CRYPTO_HMAC_SHA1,
    SLOT_CRYPTO_RANDOM_BYTES,
    SLOT_CRYPTO_DERIVE_KEY,
    SLOT_CRYPTO_AES_NEW,
    SLOT_CRYPTO_AES_UPDATE,
    SLOT_CRYPTO_AES_FREE,
    SLOT_CRYPTO_ECDH_X25519,
    SLOT_CRYPTO_ECDH_P256,
    SLOT_CRYPTO_ECDH_GET_PUBLIC_KEY,
    SLOT_CRYPTO_ECDH_COMPUTE_SHARED,
    SLOT_CRYPTO_ECDH_FREE,
    SLOT_CRYPTO_RSA_VERIFY,
    SLOT_CRYPTO_ECDSA_P256_VERIFY,
    SLOT_CRYPTO_END,

    // picocalc_graphics_t (10 functions)
    SLOT_GFX_LOAD = SLOT_CRYPTO_END,
    SLOT_GFX_NEW_BLANK,
    SLOT_GFX_FREE,
    SLOT_GFX_WIDTH,
    SLOT_GFX_HEIGHT,
    SLOT_GFX_PIXELS,
    SLOT_GFX_SET_TRANSPARENT_COLOR,
    SLOT_GFX_DRAW,
    SLOT_GFX_DRAW_REGION,
    SLOT_GFX_DRAW_SCALED,
    SLOT_GRAPHICS_END,

    // picocalc_video_t (22 functions)
    SLOT_VIDEO_NEW_PLAYER = SLOT_GRAPHICS_END,
    SLOT_VIDEO_FREE,
    SLOT_VIDEO_LOAD,
    SLOT_VIDEO_PLAY,
    SLOT_VIDEO_PAUSE,
    SLOT_VIDEO_RESUME,
    SLOT_VIDEO_STOP,
    SLOT_VIDEO_UPDATE,
    SLOT_VIDEO_SEEK,
    SLOT_VIDEO_GET_FPS,
    SLOT_VIDEO_GET_SIZE,
    SLOT_VIDEO_IS_PLAYING,
    SLOT_VIDEO_IS_PAUSED,
    SLOT_VIDEO_SET_LOOP,
    SLOT_VIDEO_SET_AUTO_FLUSH,
    SLOT_VIDEO_HAS_AUDIO,
    SLOT_VIDEO_SET_VOLUME,
    SLOT_VIDEO_GET_VOLUME,
    SLOT_VIDEO_SET_MUTED,
    SLOT_VIDEO_GET_MUTED,
    SLOT_VIDEO_GET_DROPPED_FRAMES,
    SLOT_VIDEO_RESET_STATS,
    SLOT_VIDEO_END,

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

// Native ARM apps write big-endian RGB565 (matching the ST7365P display).
// The simulator's SDL2 expects host-byte-order (little-endian on x86/ARM64).
// Byte-swap after copying from emulated memory to host back buffer.
static void byteswap_rgb565(uint16_t *buf, int count) {
    for (int i = 0; i < count; i++) {
        uint16_t p = buf[i];
        buf[i] = (p >> 8) | (p << 8);
    }
}

static void tramp_display_flush(uc_engine *uc) {
    // Copy the emulated framebuffer into the simulator's back buffer,
    // then present via the normal display_flush() path.
    uint16_t *back = display_get_back_buffer();
    uc_mem_read(uc, EMU_FB_BASE, back, 320 * 320 * sizeof(uint16_t));
    byteswap_rgb565(back, 320 * 320);
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
    byteswap_rgb565(buf, pixels);

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
    byteswap_rgb565(back + y0 * 320, (y1 - y0) * 320);
    display_flush();
}

static void tramp_display_flush_region(uc_engine *uc) {
    // flushRegion — copy entire emulated FB, then flush
    uint16_t *back = display_get_back_buffer();
    uc_mem_read(uc, EMU_FB_BASE, back, 320 * 320 * sizeof(uint16_t));
    byteswap_rgb565(back, 320 * 320);
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
    uint32_t handle = f ? handle_wrap(f) : 0;
    if (!f) {
        fprintf(stderr, "[TRAMP] fs_open('%s', '%s') -> FAIL\n",
                path ? path : "(null)", mode ? mode : "(null)");
    }
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

        // Handle length modifiers (ignored — ARM32 args are all 32-bit)
        if (*p == 'l') { p++; if (*p == 'l') p++; }
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
// TCP trampoline handlers
// =============================================================================

static void tramp_tcp_connect(uc_engine *uc) {
    uint32_t host_addr = read_reg(uc, UC_ARM_REG_R0);
    uint16_t port = (uint16_t)read_reg(uc, UC_ARM_REG_R1);
    bool use_ssl = (bool)read_reg(uc, UC_ARM_REG_R2);
    char *host = uc_read_string(uc, host_addr);
    tcp_conn_t *c = tcp_alloc();
    if (c && host) {
        tcp_connect(c, host, port, use_ssl);
    }
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(c));
}

static void tramp_tcp_write(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t buf_addr = read_reg(uc, UC_ARM_REG_R1);
    int len = (int)read_reg(uc, UC_ARM_REG_R2);
    tcp_conn_t *c = handle_unwrap(handle);
    if (!c || len <= 0) { write_reg(uc, UC_ARM_REG_R0, (uint32_t)-1); return; }
    void *buf = malloc(len);
    if (!buf) { write_reg(uc, UC_ARM_REG_R0, (uint32_t)-1); return; }
    uc_mem_read(uc, buf_addr, buf, len);
    int result = tcp_write(c, buf, len);
    free(buf);
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)result);
}

static void tramp_tcp_read(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t buf_addr = read_reg(uc, UC_ARM_REG_R1);
    int len = (int)read_reg(uc, UC_ARM_REG_R2);
    tcp_conn_t *c = handle_unwrap(handle);
    if (!c || len <= 0) { write_reg(uc, UC_ARM_REG_R0, 0); return; }
    void *buf = malloc(len);
    if (!buf) { write_reg(uc, UC_ARM_REG_R0, 0); return; }
    int result = tcp_read(c, buf, len);
    if (result > 0) uc_mem_write(uc, buf_addr, buf, result);
    free(buf);
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)result);
}

static void tramp_tcp_close(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    tcp_conn_t *c = handle_unwrap(handle);
    if (c) { tcp_close(c); tcp_free(c); }
    handle_free(handle);
}

static void tramp_tcp_available(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    tcp_conn_t *c = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, c ? tcp_bytes_available(c) : 0);
}

static void tramp_tcp_get_error(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    tcp_conn_t *c = handle_unwrap(handle);
    const char *err = c ? tcp_get_error(c) : NULL;
    write_reg(uc, UC_ARM_REG_R0, err ? arena_write_string(uc, err) : 0);
}

static void tramp_tcp_get_events(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    tcp_conn_t *c = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, c ? tcp_take_pending(c) : 0);
}

// =============================================================================
// UI trampoline handlers
// =============================================================================

static void tramp_ui_text_input(uc_engine *uc) {
    // textInput(title, prompt, initial, out, out_len) -> bool
    // This is a blocking modal — in the emulator we just return false
    printf("[UNICORN] ui->textInput() — modal not supported in emulation\n");
    write_reg(uc, UC_ARM_REG_R0, 0);
}

static void tramp_ui_text_input_simple(uc_engine *uc) {
    printf("[UNICORN] ui->textInputSimple() — modal not supported in emulation\n");
    write_reg(uc, UC_ARM_REG_R0, 0);
}

static void tramp_ui_confirm(uc_engine *uc) {
    printf("[UNICORN] ui->confirm() — modal not supported in emulation\n");
    write_reg(uc, UC_ARM_REG_R0, 0);
}

// =============================================================================
// HTTP trampoline handlers
// =============================================================================

static void tramp_http_new_conn(uc_engine *uc) {
    uint32_t server_addr = read_reg(uc, UC_ARM_REG_R0);
    uint16_t port = (uint16_t)read_reg(uc, UC_ARM_REG_R1);
    bool use_ssl = (bool)read_reg(uc, UC_ARM_REG_R2);
    char *server = uc_read_string(uc, server_addr);
    http_conn_t *c = http_alloc();
    if (c && server) {
        strncpy(c->server, server, HTTP_SERVER_MAX - 1);
        c->port = port ? port : (use_ssl ? 443 : 80);
        c->use_ssl = use_ssl;
    }
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(c));
}

static void tramp_http_get(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R1);
    uint32_t hdrs_addr = read_reg(uc, UC_ARM_REG_R2);
    http_conn_t *c = handle_unwrap(handle);
    char *path = uc_read_string(uc, path_addr);
    char *hdrs = hdrs_addr ? uc_read_string(uc, hdrs_addr) : NULL;
    if (c && path) http_get(c, path, hdrs);
}

static void tramp_http_post(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R1);
    uint32_t hdrs_addr = read_reg(uc, UC_ARM_REG_R2);
    uint32_t body_addr = read_reg(uc, UC_ARM_REG_R3);
    uint32_t body_len = read_stack_arg(uc, 0);
    http_conn_t *c = handle_unwrap(handle);
    char *path = uc_read_string(uc, path_addr);
    char *hdrs = hdrs_addr ? uc_read_string(uc, hdrs_addr) : NULL;
    char *body = NULL;
    if (body_addr && body_len > 0) {
        body = malloc(body_len + 1);
        if (body) {
            uc_mem_read(uc, body_addr, body, body_len);
            body[body_len] = '\0';
        }
    }
    if (c && path) http_post(c, path, hdrs, body, body_len);
    free(body);
}

static void tramp_http_read(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t buf_addr = read_reg(uc, UC_ARM_REG_R1);
    uint32_t len = read_reg(uc, UC_ARM_REG_R2);
    http_conn_t *c = handle_unwrap(handle);
    if (!c || len == 0) { write_reg(uc, UC_ARM_REG_R0, 0); return; }
    uint8_t *buf = malloc(len);
    if (!buf) { write_reg(uc, UC_ARM_REG_R0, 0); return; }
    uint32_t result = http_read(c, buf, len);
    if (result > 0) uc_mem_write(uc, buf_addr, buf, result);
    free(buf);
    write_reg(uc, UC_ARM_REG_R0, result);
}

static void tramp_http_available(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    http_conn_t *c = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, c ? http_bytes_available(c) : 0);
}

static void tramp_http_close(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    http_conn_t *c = handle_unwrap(handle);
    if (c) { http_close(c); http_free(c); }
    handle_free(handle);
}

static void tramp_http_get_status(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    http_conn_t *c = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, c ? (uint32_t)c->status_code : 0);
}

static void tramp_http_get_error(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    http_conn_t *c = handle_unwrap(handle);
    const char *err = (c && c->err[0]) ? c->err : NULL;
    write_reg(uc, UC_ARM_REG_R0, err ? arena_write_string(uc, err) : 0);
}

static void tramp_http_get_progress(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t recv_addr = read_reg(uc, UC_ARM_REG_R1);
    uint32_t total_addr = read_reg(uc, UC_ARM_REG_R2);
    http_conn_t *c = handle_unwrap(handle);
    int received = 0, total = 0;
    if (c) {
        received = (int)c->body_received;
        total = (int)c->content_length;
    }
    if (recv_addr) uc_mem_write(uc, recv_addr, &received, 4);
    if (total_addr) uc_mem_write(uc, total_addr, &total, 4);
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)total);
}

static void tramp_http_set_keep_alive(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    bool keep = (bool)read_reg(uc, UC_ARM_REG_R1);
    http_conn_t *c = handle_unwrap(handle);
    if (c) c->keep_alive = keep;
}

static void tramp_http_set_byte_range(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int from = (int)read_reg(uc, UC_ARM_REG_R1);
    int to = (int)read_reg(uc, UC_ARM_REG_R2);
    http_conn_t *c = handle_unwrap(handle);
    if (c) { c->range_from = from; c->range_to = to; }
}

static void tramp_http_set_connect_timeout(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int seconds = (int)read_reg(uc, UC_ARM_REG_R1);
    http_conn_t *c = handle_unwrap(handle);
    if (c) c->connect_timeout_ms = seconds * 1000;
}

static void tramp_http_set_read_timeout(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int seconds = (int)read_reg(uc, UC_ARM_REG_R1);
    http_conn_t *c = handle_unwrap(handle);
    if (c) c->read_timeout_ms = seconds * 1000;
}

static void tramp_http_set_read_buffer_size(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int bytes = (int)read_reg(uc, UC_ARM_REG_R1);
    http_conn_t *c = handle_unwrap(handle);
    if (c) http_set_recv_buf(c, bytes);
}

// =============================================================================
// Soundplayer trampoline handlers
// =============================================================================

// --- Sample ---
static void tramp_snd_sample_load(uc_engine *uc) {
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R0);
    char *path = uc_read_string(uc, path_addr);
    sound_sample_t *s = sound_sample_create();
    if (s && path) {
        if (!sound_sample_load(s, path)) {
            sound_sample_destroy(s);
            s = NULL;
        }
    }
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(s));
}

static void tramp_snd_sample_free(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    sound_sample_t *s = handle_unwrap(handle);
    if (s) sound_sample_destroy(s);
    handle_free(handle);
}

// --- Sample player ---
static void tramp_snd_player_new(uc_engine *uc) {
    sound_player_t *p = sound_player_create();
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(p));
}

static void tramp_snd_player_set_sample(uc_engine *uc) {
    uint32_t ph = read_reg(uc, UC_ARM_REG_R0);
    uint32_t sh = read_reg(uc, UC_ARM_REG_R1);
    sound_player_t *p = handle_unwrap(ph);
    sound_sample_t *s = handle_unwrap(sh);
    if (p) sound_player_set_sample(p, s);
}

static void tramp_snd_player_play(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint8_t repeat = (uint8_t)read_reg(uc, UC_ARM_REG_R1);
    sound_player_t *p = handle_unwrap(handle);
    if (p) sound_player_play(p, repeat);
}

static void tramp_snd_player_stop(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    sound_player_t *p = handle_unwrap(handle);
    if (p) sound_player_stop(p);
}

static void tramp_snd_player_is_playing(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    sound_player_t *p = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, (p && sound_player_is_playing(p)) ? 1 : 0);
}

static void tramp_snd_player_get_volume(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    sound_player_t *p = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, p ? sound_player_get_volume(p) : 0);
}

static void tramp_snd_player_set_volume(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint8_t vol = (uint8_t)read_reg(uc, UC_ARM_REG_R1);
    sound_player_t *p = handle_unwrap(handle);
    if (p) sound_player_set_volume(p, vol);
}

static void tramp_snd_player_set_loop(uc_engine *uc) {
    // The sound_player doesn't have a direct set_loop, but repeat_count=0 with play means loop
    (void)uc;
}

static void tramp_snd_player_free(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    sound_player_t *p = handle_unwrap(handle);
    if (p) sound_player_destroy(p);
    handle_free(handle);
}

// --- File player ---
static void tramp_snd_fp_new(uc_engine *uc) {
    fileplayer_t *fp = fileplayer_create();
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(fp));
}

static void tramp_snd_fp_load(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R1);
    fileplayer_t *fp = handle_unwrap(handle);
    char *path = uc_read_string(uc, path_addr);
    if (fp && path) fileplayer_load(fp, path);
}

static void tramp_snd_fp_play(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint8_t repeat = (uint8_t)read_reg(uc, UC_ARM_REG_R1);
    fileplayer_t *fp = handle_unwrap(handle);
    if (fp) fileplayer_play(fp, repeat);
}

static void tramp_snd_fp_stop(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    fileplayer_t *fp = handle_unwrap(handle);
    if (fp) fileplayer_stop(fp);
}

static void tramp_snd_fp_pause(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    fileplayer_t *fp = handle_unwrap(handle);
    if (fp) fileplayer_pause(fp);
}

static void tramp_snd_fp_resume(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    fileplayer_t *fp = handle_unwrap(handle);
    if (fp) fileplayer_resume(fp);
}

static void tramp_snd_fp_is_playing(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    fileplayer_t *fp = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, (fp && fileplayer_is_playing(fp)) ? 1 : 0);
}

static void tramp_snd_fp_set_volume(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint8_t vol = (uint8_t)read_reg(uc, UC_ARM_REG_R1);
    fileplayer_t *fp = handle_unwrap(handle);
    if (fp) fileplayer_set_volume(fp, vol, vol);  // Set both L/R to same
}

static void tramp_snd_fp_get_volume(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    fileplayer_t *fp = handle_unwrap(handle);
    // The C API returns left channel volume
    uint8_t left = 0, right = 0;
    if (fp) fileplayer_get_volume(fp, &left, &right);
    write_reg(uc, UC_ARM_REG_R0, left);
}

static void tramp_snd_fp_get_offset(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    fileplayer_t *fp = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, fp ? fileplayer_get_offset(fp) : 0);
}

static void tramp_snd_fp_set_offset(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t pos = read_reg(uc, UC_ARM_REG_R1);
    fileplayer_t *fp = handle_unwrap(handle);
    if (fp) fileplayer_set_offset(fp, pos);
}

static void tramp_snd_fp_did_underrun(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    (void)handle;  // fileplayer_did_underrun is global
    write_reg(uc, UC_ARM_REG_R0, fileplayer_did_underrun() ? 1 : 0);
}

static void tramp_snd_fp_free(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    fileplayer_t *fp = handle_unwrap(handle);
    if (fp) fileplayer_destroy(fp);
    handle_free(handle);
}

// --- MP3 player ---
static void tramp_snd_mp3_new(uc_engine *uc) {
    mp3_player_t *mp = mp3_player_create();
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(mp));
}

static void tramp_snd_mp3_load(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R1);
    mp3_player_t *mp = handle_unwrap(handle);
    char *path = uc_read_string(uc, path_addr);
    if (mp && path) mp3_player_load(mp, path);
}

static void tramp_snd_mp3_play(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint8_t repeat = (uint8_t)read_reg(uc, UC_ARM_REG_R1);
    mp3_player_t *mp = handle_unwrap(handle);
    if (mp) mp3_player_play(mp, repeat);
}

static void tramp_snd_mp3_stop(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    mp3_player_t *mp = handle_unwrap(handle);
    if (mp) mp3_player_stop(mp);
}

static void tramp_snd_mp3_pause(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    mp3_player_t *mp = handle_unwrap(handle);
    if (mp) mp3_player_pause(mp);
}

static void tramp_snd_mp3_resume(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    mp3_player_t *mp = handle_unwrap(handle);
    if (mp) mp3_player_resume(mp);
}

static void tramp_snd_mp3_is_playing(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    mp3_player_t *mp = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, (mp && mp3_player_is_playing(mp)) ? 1 : 0);
}

static void tramp_snd_mp3_set_volume(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint8_t vol = (uint8_t)read_reg(uc, UC_ARM_REG_R1);
    mp3_player_t *mp = handle_unwrap(handle);
    if (mp) mp3_player_set_volume(mp, vol);
}

static void tramp_snd_mp3_get_volume(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    mp3_player_t *mp = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, mp ? mp3_player_get_volume(mp) : 0);
}

static void tramp_snd_mp3_set_loop(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    bool loop = (bool)read_reg(uc, UC_ARM_REG_R1);
    mp3_player_t *mp = handle_unwrap(handle);
    if (mp) mp3_player_set_loop(mp, loop);
}

static void tramp_snd_mp3_free(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    mp3_player_t *mp = handle_unwrap(handle);
    if (mp) mp3_player_destroy(mp);
    handle_free(handle);
}

// =============================================================================
// Appconfig trampoline handlers
// =============================================================================

static void tramp_appconfig_load(uc_engine *uc) {
    uint32_t id_addr = read_reg(uc, UC_ARM_REG_R0);
    char *app_id = uc_read_string(uc, id_addr);
    write_reg(uc, UC_ARM_REG_R0, appconfig_load(app_id ? app_id : "") ? 1 : 0);
}

static void tramp_appconfig_save(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, appconfig_save() ? 1 : 0);
}

static void tramp_appconfig_get(uc_engine *uc) {
    uint32_t key_addr = read_reg(uc, UC_ARM_REG_R0);
    uint32_t fb_addr = read_reg(uc, UC_ARM_REG_R1);
    char *key = uc_read_string(uc, key_addr);
    char *fallback = fb_addr ? uc_read_string(uc, fb_addr) : NULL;
    const char *val = appconfig_get(key ? key : "", fallback);
    write_reg(uc, UC_ARM_REG_R0, val ? arena_write_string(uc, val) : 0);
}

static void tramp_appconfig_set(uc_engine *uc) {
    uint32_t key_addr = read_reg(uc, UC_ARM_REG_R0);
    uint32_t val_addr = read_reg(uc, UC_ARM_REG_R1);
    char *key = uc_read_string(uc, key_addr);
    char *val = uc_read_string(uc, val_addr);
    if (key && val) appconfig_set(key, val);
}

static void tramp_appconfig_clear(uc_engine *uc) {
    (void)uc;
    appconfig_clear();
}

static void tramp_appconfig_reset(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, appconfig_reset() ? 1 : 0);
}

static void tramp_appconfig_get_app_id(uc_engine *uc) {
    const char *id = appconfig_get_app_id();
    write_reg(uc, UC_ARM_REG_R0, id ? arena_write_string(uc, id) : 0);
}

// =============================================================================
// Terminal trampoline handlers
// =============================================================================

static void tramp_term_create(uc_engine *uc) {
    int cols = (int)read_reg(uc, UC_ARM_REG_R0);
    int rows = (int)read_reg(uc, UC_ARM_REG_R1);
    int scrollback = (int)read_reg(uc, UC_ARM_REG_R2);
    terminal_t *t = terminal_new(cols, rows, scrollback);
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(t));
}

static void tramp_term_free(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_free(t);
    handle_free(handle);
}

static void tramp_term_clear(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_clear(t);
}

static void tramp_term_write(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t str_addr = read_reg(uc, UC_ARM_REG_R1);
    terminal_t *t = handle_unwrap(handle);
    char *str = uc_read_string(uc, str_addr);
    if (t && str) terminal_putString(t, str);
}

static void tramp_term_put_char(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    char c = (char)read_reg(uc, UC_ARM_REG_R1);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_putChar(t, c);
}

static void tramp_term_set_cursor(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int x = (int)read_reg(uc, UC_ARM_REG_R1);
    int y = (int)read_reg(uc, UC_ARM_REG_R2);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_setCursor(t, x, y);
}

static void tramp_term_get_cursor(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t ox_addr = read_reg(uc, UC_ARM_REG_R1);
    uint32_t oy_addr = read_reg(uc, UC_ARM_REG_R2);
    terminal_t *t = handle_unwrap(handle);
    int x = 0, y = 0;
    if (t) terminal_getCursor(t, &x, &y);
    if (ox_addr) uc_mem_write(uc, ox_addr, &x, 4);
    if (oy_addr) uc_mem_write(uc, oy_addr, &y, 4);
}

static void tramp_term_set_colors(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint16_t fg = (uint16_t)read_reg(uc, UC_ARM_REG_R1);
    uint16_t bg = (uint16_t)read_reg(uc, UC_ARM_REG_R2);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_setColors(t, fg, bg);
}

static void tramp_term_get_colors(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t fg_addr = read_reg(uc, UC_ARM_REG_R1);
    uint32_t bg_addr = read_reg(uc, UC_ARM_REG_R2);
    terminal_t *t = handle_unwrap(handle);
    uint16_t fg = 0xFFFF, bg = 0x0000;
    if (t) terminal_getColors(t, &fg, &bg);
    if (fg_addr) uc_mem_write(uc, fg_addr, &fg, 2);
    if (bg_addr) uc_mem_write(uc, bg_addr, &bg, 2);
}

static void tramp_term_scroll(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int lines = (int)read_reg(uc, UC_ARM_REG_R1);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_scroll(t, lines);
}

static void tramp_term_render(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_render(t);
}

static void tramp_term_render_dirty(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_renderDirty(t);
}

static void tramp_term_get_cols(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, t ? (uint32_t)terminal_getCols(t) : 0);
}

static void tramp_term_get_rows(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, t ? (uint32_t)terminal_getRows(t) : 0);
}

static void tramp_term_set_cursor_visible(uc_engine *uc) {
    // setCursorVisible doesn't take terminal_t* — it's global
    bool visible = (bool)read_reg(uc, UC_ARM_REG_R0);
    extern void terminal_setCursorVisible(bool);
    terminal_setCursorVisible(visible);
}

static void tramp_term_set_cursor_blink(uc_engine *uc) {
    bool blink = (bool)read_reg(uc, UC_ARM_REG_R0);
    extern void terminal_setCursorBlink(bool);
    terminal_setCursorBlink(blink);
}

static void tramp_term_mark_all_dirty(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_markAllDirty(t);
}

static void tramp_term_is_full_dirty(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, (t && terminal_isFullDirty(t)) ? 1 : 0);
}

static void tramp_term_get_dirty_range(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint32_t first_addr = read_reg(uc, UC_ARM_REG_R1);
    uint32_t last_addr = read_reg(uc, UC_ARM_REG_R2);
    terminal_t *t = handle_unwrap(handle);
    int first = 0, last = 0;
    if (t) terminal_getDirtyRange(t, &first, &last);
    if (first_addr) uc_mem_write(uc, first_addr, &first, 4);
    if (last_addr) uc_mem_write(uc, last_addr, &last, 4);
}

static void tramp_term_get_scrollback_count(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, t ? (uint32_t)terminal_getScrollbackCount(t) : 0);
}

static void tramp_term_set_scrollback_offset(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int offset = (int)read_reg(uc, UC_ARM_REG_R1);
    terminal_t *t = handle_unwrap(handle);
    if (t) terminal_setScrollbackOffset(t, offset);
}

static void tramp_term_get_scrollback_offset(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    terminal_t *t = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, t ? (uint32_t)terminal_getScrollbackOffset(t) : 0);
}

static void tramp_term_get_scrollback_line(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int line = (int)read_reg(uc, UC_ARM_REG_R1);
    uint32_t out_addr = read_reg(uc, UC_ARM_REG_R2);
    terminal_t *t = handle_unwrap(handle);
    if (t && out_addr) {
        int cols = terminal_getCols(t);
        uint16_t *cells = malloc(cols * sizeof(uint16_t));
        if (cells) {
            terminal_getScrollbackLine(t, line, cells);
            uc_mem_write(uc, out_addr, cells, cols * sizeof(uint16_t));
            free(cells);
        }
    }
}

static void tramp_term_get_scrollback_line_colors(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int line = (int)read_reg(uc, UC_ARM_REG_R1);
    uint32_t fg_addr = read_reg(uc, UC_ARM_REG_R2);
    uint32_t bg_addr = read_reg(uc, UC_ARM_REG_R3);
    terminal_t *t = handle_unwrap(handle);
    if (t) {
        int cols = terminal_getCols(t);
        uint16_t *fg = malloc(cols * sizeof(uint16_t));
        uint16_t *bg = malloc(cols * sizeof(uint16_t));
        if (fg && bg) {
            terminal_getScrollbackLineColors(t, line, fg, bg);
            if (fg_addr) uc_mem_write(uc, fg_addr, fg, cols * sizeof(uint16_t));
            if (bg_addr) uc_mem_write(uc, bg_addr, bg, cols * sizeof(uint16_t));
        }
        free(fg);
        free(bg);
    }
}

// Terminal line numbers, scrollbar, word wrap — simple setter/getter trampolines
static void tramp_term_set_line_numbers(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setLineNumbers(t, (bool)read_reg(uc, UC_ARM_REG_R1));
}
static void tramp_term_set_line_number_start(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setLineNumberStart(t, (int)read_reg(uc, UC_ARM_REG_R1));
}
static void tramp_term_set_line_number_cols(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setLineNumberCols(t, (int)read_reg(uc, UC_ARM_REG_R1));
}
static void tramp_term_set_line_number_colors(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setLineNumberColors(t, (uint16_t)read_reg(uc, UC_ARM_REG_R1),
                                            (uint16_t)read_reg(uc, UC_ARM_REG_R2));
}
static void tramp_term_get_content_cols(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    write_reg(uc, UC_ARM_REG_R0, t ? (uint32_t)terminal_getContentCols(t) : 0);
}
static void tramp_term_set_scrollbar(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setScrollbar(t, (bool)read_reg(uc, UC_ARM_REG_R1));
}
static void tramp_term_set_scrollbar_colors(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setScrollbarColors(t, (uint16_t)read_reg(uc, UC_ARM_REG_R1),
                                           (uint16_t)read_reg(uc, UC_ARM_REG_R2));
}
static void tramp_term_set_scrollbar_width(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setScrollbarWidth(t, (int)read_reg(uc, UC_ARM_REG_R1));
}
static void tramp_term_set_scroll_info(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setScrollInfo(t, (int)read_reg(uc, UC_ARM_REG_R1),
                                     (int)read_reg(uc, UC_ARM_REG_R2));
}
static void tramp_term_set_render_bounds(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setRenderBounds(t, (int)read_reg(uc, UC_ARM_REG_R1),
                                       (int)read_reg(uc, UC_ARM_REG_R2));
}
static void tramp_term_set_word_wrap(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setWordWrap(t, (bool)read_reg(uc, UC_ARM_REG_R1));
}
static void tramp_term_set_word_wrap_column(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setWordWrapColumn(t, (int)read_reg(uc, UC_ARM_REG_R1));
}
static void tramp_term_set_wrap_indicator(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (t) terminal_setWrapIndicator(t, (bool)read_reg(uc, UC_ARM_REG_R1));
}
static void tramp_term_get_word_wrap(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    write_reg(uc, UC_ARM_REG_R0, (t && terminal_getWordWrap(t)) ? 1 : 0);
}
static void tramp_term_get_visual_row_count(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    write_reg(uc, UC_ARM_REG_R0, t ? (uint32_t)terminal_getVisualRowCount(t) : 0);
}
static void tramp_term_logical_to_visual(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    int log_x = (int)read_reg(uc, UC_ARM_REG_R1);
    int log_y = (int)read_reg(uc, UC_ARM_REG_R2);
    uint32_t out_addr = read_reg(uc, UC_ARM_REG_R3);  // packed: vis_x ptr
    uint32_t vy_addr = read_stack_arg(uc, 0);          // vis_y ptr
    int vx = 0, vy = 0;
    if (t) terminal_logicalToVisual(t, log_x, log_y, &vx, &vy);
    if (out_addr) uc_mem_write(uc, out_addr, &vx, 4);
    if (vy_addr) uc_mem_write(uc, vy_addr, &vy, 4);
}
static void tramp_term_visual_to_logical(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    int vis_x = (int)read_reg(uc, UC_ARM_REG_R1);
    int vis_y = (int)read_reg(uc, UC_ARM_REG_R2);
    uint32_t ox_addr = read_reg(uc, UC_ARM_REG_R3);
    uint32_t oy_addr = read_stack_arg(uc, 0);
    int lx = 0, ly = 0;
    if (t) terminal_visualToLogical(t, vis_x, vis_y, &lx, &ly);
    if (ox_addr) uc_mem_write(uc, ox_addr, &lx, 4);
    if (oy_addr) uc_mem_write(uc, oy_addr, &ly, 4);
}
static void tramp_term_calculate_line_wraps(uc_engine *uc) {
    terminal_t *t = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    int logical_line = (int)read_reg(uc, UC_ARM_REG_R1);
    uint32_t seg_addr = read_reg(uc, UC_ARM_REG_R2);
    int max_seg = (int)read_reg(uc, UC_ARM_REG_R3);
    int result = 0;
    if (t && seg_addr && max_seg > 0) {
        int *segs = malloc(max_seg * sizeof(int));
        if (segs) {
            result = terminal_calculateLineWraps(t, logical_line, segs, max_seg);
            uc_mem_write(uc, seg_addr, segs, max_seg * sizeof(int));
            free(segs);
        }
    }
    write_reg(uc, UC_ARM_REG_R0, (uint32_t)result);
}

// =============================================================================
// Graphics trampoline handlers
// =============================================================================

static void tramp_gfx_load(uc_engine *uc) {
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R0);
    char *path = uc_read_string(uc, path_addr);
    pc_image_t *img = path ? image_load(path) : NULL;
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(img));
}

static void tramp_gfx_new_blank(uc_engine *uc) {
    int w = (int)read_reg(uc, UC_ARM_REG_R0);
    int h = (int)read_reg(uc, UC_ARM_REG_R1);
    pc_image_t *img = image_new_blank(w, h);
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(img));
}

static void tramp_gfx_free(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    pc_image_t *img = handle_unwrap(handle);
    if (img) {
        extern void image_free(pc_image_t *);
        image_free(img);
    }
    handle_free(handle);
}

static void tramp_gfx_width(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    pc_image_t *img = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, img ? (uint32_t)img->w : 0);
}

static void tramp_gfx_height(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    pc_image_t *img = handle_unwrap(handle);
    write_reg(uc, UC_ARM_REG_R0, img ? (uint32_t)img->h : 0);
}

static void tramp_gfx_pixels(uc_engine *uc) {
    // Returns a pointer to pixel data — problematic for emulated code.
    // We'd need to copy pixel data into emulated memory.
    // For now, return 0 (NULL).
    write_reg(uc, UC_ARM_REG_R0, 0);
}

static void tramp_gfx_set_transparent_color(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    uint16_t color = (uint16_t)read_reg(uc, UC_ARM_REG_R1);
    pc_image_t *img = handle_unwrap(handle);
    if (img) img->transparent_color = color;
}

static void tramp_gfx_draw(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int x = (int)read_reg(uc, UC_ARM_REG_R1);
    int y = (int)read_reg(uc, UC_ARM_REG_R2);
    pc_image_t *img = handle_unwrap(handle);
    if (img) image_draw(img, x, y);
}

static void tramp_gfx_draw_region(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int sx = (int)read_reg(uc, UC_ARM_REG_R1);
    int sy = (int)read_reg(uc, UC_ARM_REG_R2);
    int sw = (int)read_reg(uc, UC_ARM_REG_R3);
    int sh = (int)read_stack_arg(uc, 0);
    int dx = (int)read_stack_arg(uc, 1);
    int dy = (int)read_stack_arg(uc, 2);
    pc_image_t *img = handle_unwrap(handle);
    if (img) image_draw_region(img, sx, sy, sw, sh, dx, dy);
}

static void tramp_gfx_draw_scaled(uc_engine *uc) {
    uint32_t handle = read_reg(uc, UC_ARM_REG_R0);
    int x = (int)read_reg(uc, UC_ARM_REG_R1);
    int y = (int)read_reg(uc, UC_ARM_REG_R2);
    int dw = (int)read_reg(uc, UC_ARM_REG_R3);
    int dh = (int)read_stack_arg(uc, 0);
    pc_image_t *img = handle_unwrap(handle);
    if (img) image_draw_scaled(img, x, y, dw, dh);
}

// =============================================================================
// Video trampoline handlers
// =============================================================================

static void tramp_video_new_player(uc_engine *uc) {
    void *vp = video_player_create();
    write_reg(uc, UC_ARM_REG_R0, handle_wrap(vp));
}
static void tramp_video_free(uc_engine *uc) {
    uint32_t h = read_reg(uc, UC_ARM_REG_R0);
    void *vp = handle_unwrap(h);
    if (vp) video_player_destroy(vp);
    handle_free(h);
}
static void tramp_video_load(uc_engine *uc) {
    uint32_t h = read_reg(uc, UC_ARM_REG_R0);
    uint32_t path_addr = read_reg(uc, UC_ARM_REG_R1);
    void *vp = handle_unwrap(h);
    char *path = uc_read_string(uc, path_addr);
    write_reg(uc, UC_ARM_REG_R0, (vp && path) ? (uint32_t)video_player_load(vp, path) : 0);
}
static void tramp_video_play(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (vp) video_player_play(vp);
}
static void tramp_video_pause(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (vp) video_player_pause(vp);
}
static void tramp_video_resume(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (vp) video_player_resume(vp);
}
static void tramp_video_stop(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (vp) video_player_stop(vp);
}
static void tramp_video_update(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (vp) video_player_update(vp);
    write_reg(uc, UC_ARM_REG_R0, 0);  // bool return
}
static void tramp_video_seek(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    uint32_t frame = read_reg(uc, UC_ARM_REG_R1);
    if (vp) video_player_seek(vp, (float)frame);
}
static void tramp_video_get_fps(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    // Return as fixed-point * 100 since we can't return float via r0 easily
    float fps = vp ? video_player_get_fps(vp) : 0.0f;
    // For ARM, floats go in s0 but our SVC mechanism only uses r0.
    // Copy raw float bits.
    uint32_t raw;
    memcpy(&raw, &fps, 4);
    write_reg(uc, UC_ARM_REG_R0, raw);
}
static void tramp_video_get_size(uc_engine *uc) {
    (void)handle_unwrap(read_reg(uc, UC_ARM_REG_R0));  // player handle (unused for stub)
    uint32_t w_addr = read_reg(uc, UC_ARM_REG_R1);
    uint32_t h_addr = read_reg(uc, UC_ARM_REG_R2);
    uint32_t w = 0, h = 0;
    // video_player_get_size not in stubs — write zeros
    if (w_addr) uc_mem_write(uc, w_addr, &w, 4);
    if (h_addr) uc_mem_write(uc, h_addr, &h, 4);
}
static void tramp_video_is_playing(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, 0);
}
static void tramp_video_is_paused(uc_engine *uc) {
    write_reg(uc, UC_ARM_REG_R0, 0);
}
static void tramp_video_set_loop(uc_engine *uc) { (void)uc; }
static void tramp_video_set_auto_flush(uc_engine *uc) { (void)uc; }
static void tramp_video_has_audio(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    write_reg(uc, UC_ARM_REG_R0, vp ? (uint32_t)video_player_has_audio(vp) : 0);
}
static void tramp_video_set_volume(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    uint8_t vol = (uint8_t)read_reg(uc, UC_ARM_REG_R1);
    if (vp) video_player_set_audio_volume(vp, vol);
}
static void tramp_video_get_volume(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    write_reg(uc, UC_ARM_REG_R0, vp ? video_player_get_audio_volume(vp) : 100);
}
static void tramp_video_set_muted(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    bool muted = (bool)read_reg(uc, UC_ARM_REG_R1);
    if (vp) video_player_set_audio_muted(vp, muted);
}
static void tramp_video_get_muted(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    write_reg(uc, UC_ARM_REG_R0, vp ? (uint32_t)video_player_get_audio_muted(vp) : 0);
}
static void tramp_video_get_dropped_frames(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    write_reg(uc, UC_ARM_REG_R0, vp ? (uint32_t)video_player_get_dropped_frames(vp) : 0);
}
static void tramp_video_reset_stats(uc_engine *uc) {
    void *vp = handle_unwrap(read_reg(uc, UC_ARM_REG_R0));
    if (vp) video_player_reset_stats(vp);
}

// =============================================================================
// Crypto trampoline handlers (stubs — crypto not yet in simulator)
// =============================================================================

static void tramp_crypto_random_bytes(uc_engine *uc) {
    uint32_t buf_addr = read_reg(uc, UC_ARM_REG_R0);
    uint32_t len = read_reg(uc, UC_ARM_REG_R1);
    if (buf_addr && len > 0 && len <= 4096) {
        // Use /dev/urandom for host-side random bytes
        uint8_t *buf = malloc(len);
        if (buf) {
            FILE *f = fopen("/dev/urandom", "rb");
            if (f) {
                fread(buf, 1, len, f);
                fclose(f);
            } else {
                // Fallback: fill with rand()
                for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)rand();
            }
            uc_mem_write(uc, buf_addr, buf, len);
            free(buf);
        }
    }
}

// =============================================================================
// Generic stub handler for unimplemented functions
// =============================================================================

static const char *s_stub_names[SLOT_TOTAL_COUNT];

static void tramp_stub(uc_engine *uc, uint32_t slot) {
    if (slot < SLOT_TOTAL_COUNT && s_stub_names[slot]) {
        printf("[UNICORN] STUB: %s() not implemented\n", s_stub_names[slot]);
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

    // TCP
    s_dispatch[SLOT_TCP_CONNECT]    = tramp_tcp_connect;
    s_dispatch[SLOT_TCP_WRITE]      = tramp_tcp_write;
    s_dispatch[SLOT_TCP_READ]       = tramp_tcp_read;
    s_dispatch[SLOT_TCP_CLOSE]      = tramp_tcp_close;
    s_dispatch[SLOT_TCP_AVAILABLE]  = tramp_tcp_available;
    s_dispatch[SLOT_TCP_GET_ERROR]  = tramp_tcp_get_error;
    s_dispatch[SLOT_TCP_GET_EVENTS] = tramp_tcp_get_events;

    // UI
    s_dispatch[SLOT_UI_TEXT_INPUT]        = tramp_ui_text_input;
    s_dispatch[SLOT_UI_TEXT_INPUT_SIMPLE] = tramp_ui_text_input_simple;
    s_dispatch[SLOT_UI_CONFIRM]          = tramp_ui_confirm;

    // Terminal
    s_dispatch[SLOT_TERM_CREATE]                   = tramp_term_create;
    s_dispatch[SLOT_TERM_FREE]                     = tramp_term_free;
    s_dispatch[SLOT_TERM_CLEAR]                    = tramp_term_clear;
    s_dispatch[SLOT_TERM_WRITE]                    = tramp_term_write;
    s_dispatch[SLOT_TERM_PUT_CHAR]                 = tramp_term_put_char;
    s_dispatch[SLOT_TERM_SET_CURSOR]               = tramp_term_set_cursor;
    s_dispatch[SLOT_TERM_GET_CURSOR]               = tramp_term_get_cursor;
    s_dispatch[SLOT_TERM_SET_COLORS]               = tramp_term_set_colors;
    s_dispatch[SLOT_TERM_GET_COLORS]               = tramp_term_get_colors;
    s_dispatch[SLOT_TERM_SCROLL]                   = tramp_term_scroll;
    s_dispatch[SLOT_TERM_RENDER]                   = tramp_term_render;
    s_dispatch[SLOT_TERM_RENDER_DIRTY]             = tramp_term_render_dirty;
    s_dispatch[SLOT_TERM_GET_COLS]                 = tramp_term_get_cols;
    s_dispatch[SLOT_TERM_GET_ROWS]                 = tramp_term_get_rows;
    s_dispatch[SLOT_TERM_SET_CURSOR_VISIBLE]       = tramp_term_set_cursor_visible;
    s_dispatch[SLOT_TERM_SET_CURSOR_BLINK]         = tramp_term_set_cursor_blink;
    s_dispatch[SLOT_TERM_MARK_ALL_DIRTY]           = tramp_term_mark_all_dirty;
    s_dispatch[SLOT_TERM_IS_FULL_DIRTY]            = tramp_term_is_full_dirty;
    s_dispatch[SLOT_TERM_GET_DIRTY_RANGE]          = tramp_term_get_dirty_range;
    s_dispatch[SLOT_TERM_GET_SCROLLBACK_COUNT]     = tramp_term_get_scrollback_count;
    s_dispatch[SLOT_TERM_SET_SCROLLBACK_OFFSET]    = tramp_term_set_scrollback_offset;
    s_dispatch[SLOT_TERM_GET_SCROLLBACK_OFFSET]    = tramp_term_get_scrollback_offset;
    s_dispatch[SLOT_TERM_GET_SCROLLBACK_LINE]      = tramp_term_get_scrollback_line;
    s_dispatch[SLOT_TERM_GET_SCROLLBACK_LINE_COLORS]= tramp_term_get_scrollback_line_colors;
    s_dispatch[SLOT_TERM_SET_LINE_NUMBERS]         = tramp_term_set_line_numbers;
    s_dispatch[SLOT_TERM_SET_LINE_NUMBER_START]    = tramp_term_set_line_number_start;
    s_dispatch[SLOT_TERM_SET_LINE_NUMBER_COLS]     = tramp_term_set_line_number_cols;
    s_dispatch[SLOT_TERM_SET_LINE_NUMBER_COLORS]   = tramp_term_set_line_number_colors;
    s_dispatch[SLOT_TERM_GET_CONTENT_COLS]         = tramp_term_get_content_cols;
    s_dispatch[SLOT_TERM_SET_SCROLLBAR]            = tramp_term_set_scrollbar;
    s_dispatch[SLOT_TERM_SET_SCROLLBAR_COLORS]     = tramp_term_set_scrollbar_colors;
    s_dispatch[SLOT_TERM_SET_SCROLLBAR_WIDTH]      = tramp_term_set_scrollbar_width;
    s_dispatch[SLOT_TERM_SET_SCROLL_INFO]          = tramp_term_set_scroll_info;
    s_dispatch[SLOT_TERM_SET_RENDER_BOUNDS]        = tramp_term_set_render_bounds;
    s_dispatch[SLOT_TERM_SET_WORD_WRAP]            = tramp_term_set_word_wrap;
    s_dispatch[SLOT_TERM_SET_WORD_WRAP_COLUMN]     = tramp_term_set_word_wrap_column;
    s_dispatch[SLOT_TERM_SET_WRAP_INDICATOR]        = tramp_term_set_wrap_indicator;
    s_dispatch[SLOT_TERM_GET_WORD_WRAP]            = tramp_term_get_word_wrap;
    s_dispatch[SLOT_TERM_GET_VISUAL_ROW_COUNT]     = tramp_term_get_visual_row_count;
    s_dispatch[SLOT_TERM_LOGICAL_TO_VISUAL]        = tramp_term_logical_to_visual;
    s_dispatch[SLOT_TERM_VISUAL_TO_LOGICAL]        = tramp_term_visual_to_logical;
    s_dispatch[SLOT_TERM_CALCULATE_LINE_WRAPS]     = tramp_term_calculate_line_wraps;

    // HTTP
    s_dispatch[SLOT_HTTP_NEW_CONN]           = tramp_http_new_conn;
    s_dispatch[SLOT_HTTP_GET]                = tramp_http_get;
    s_dispatch[SLOT_HTTP_POST]               = tramp_http_post;
    s_dispatch[SLOT_HTTP_READ]               = tramp_http_read;
    s_dispatch[SLOT_HTTP_AVAILABLE]          = tramp_http_available;
    s_dispatch[SLOT_HTTP_CLOSE]              = tramp_http_close;
    s_dispatch[SLOT_HTTP_GET_STATUS]         = tramp_http_get_status;
    s_dispatch[SLOT_HTTP_GET_ERROR]          = tramp_http_get_error;
    s_dispatch[SLOT_HTTP_GET_PROGRESS]       = tramp_http_get_progress;
    s_dispatch[SLOT_HTTP_SET_KEEP_ALIVE]     = tramp_http_set_keep_alive;
    s_dispatch[SLOT_HTTP_SET_BYTE_RANGE]     = tramp_http_set_byte_range;
    s_dispatch[SLOT_HTTP_SET_CONNECT_TIMEOUT]= tramp_http_set_connect_timeout;
    s_dispatch[SLOT_HTTP_SET_READ_TIMEOUT]   = tramp_http_set_read_timeout;
    s_dispatch[SLOT_HTTP_SET_READ_BUFFER_SIZE]= tramp_http_set_read_buffer_size;

    // Soundplayer
    s_dispatch[SLOT_SND_SAMPLE_LOAD]       = tramp_snd_sample_load;
    s_dispatch[SLOT_SND_SAMPLE_FREE]       = tramp_snd_sample_free;
    s_dispatch[SLOT_SND_PLAYER_NEW]        = tramp_snd_player_new;
    s_dispatch[SLOT_SND_PLAYER_SET_SAMPLE] = tramp_snd_player_set_sample;
    s_dispatch[SLOT_SND_PLAYER_PLAY]       = tramp_snd_player_play;
    s_dispatch[SLOT_SND_PLAYER_STOP]       = tramp_snd_player_stop;
    s_dispatch[SLOT_SND_PLAYER_IS_PLAYING] = tramp_snd_player_is_playing;
    s_dispatch[SLOT_SND_PLAYER_GET_VOLUME] = tramp_snd_player_get_volume;
    s_dispatch[SLOT_SND_PLAYER_SET_VOLUME] = tramp_snd_player_set_volume;
    s_dispatch[SLOT_SND_PLAYER_SET_LOOP]   = tramp_snd_player_set_loop;
    s_dispatch[SLOT_SND_PLAYER_FREE]       = tramp_snd_player_free;
    s_dispatch[SLOT_SND_FP_NEW]            = tramp_snd_fp_new;
    s_dispatch[SLOT_SND_FP_LOAD]           = tramp_snd_fp_load;
    s_dispatch[SLOT_SND_FP_PLAY]           = tramp_snd_fp_play;
    s_dispatch[SLOT_SND_FP_STOP]           = tramp_snd_fp_stop;
    s_dispatch[SLOT_SND_FP_PAUSE]          = tramp_snd_fp_pause;
    s_dispatch[SLOT_SND_FP_RESUME]         = tramp_snd_fp_resume;
    s_dispatch[SLOT_SND_FP_IS_PLAYING]     = tramp_snd_fp_is_playing;
    s_dispatch[SLOT_SND_FP_SET_VOLUME]     = tramp_snd_fp_set_volume;
    s_dispatch[SLOT_SND_FP_GET_VOLUME]     = tramp_snd_fp_get_volume;
    s_dispatch[SLOT_SND_FP_GET_OFFSET]     = tramp_snd_fp_get_offset;
    s_dispatch[SLOT_SND_FP_SET_OFFSET]     = tramp_snd_fp_set_offset;
    s_dispatch[SLOT_SND_FP_DID_UNDERRUN]   = tramp_snd_fp_did_underrun;
    s_dispatch[SLOT_SND_FP_FREE]           = tramp_snd_fp_free;
    s_dispatch[SLOT_SND_MP3_NEW]           = tramp_snd_mp3_new;
    s_dispatch[SLOT_SND_MP3_LOAD]          = tramp_snd_mp3_load;
    s_dispatch[SLOT_SND_MP3_PLAY]          = tramp_snd_mp3_play;
    s_dispatch[SLOT_SND_MP3_STOP]          = tramp_snd_mp3_stop;
    s_dispatch[SLOT_SND_MP3_PAUSE]         = tramp_snd_mp3_pause;
    s_dispatch[SLOT_SND_MP3_RESUME]        = tramp_snd_mp3_resume;
    s_dispatch[SLOT_SND_MP3_IS_PLAYING]    = tramp_snd_mp3_is_playing;
    s_dispatch[SLOT_SND_MP3_SET_VOLUME]    = tramp_snd_mp3_set_volume;
    s_dispatch[SLOT_SND_MP3_GET_VOLUME]    = tramp_snd_mp3_get_volume;
    s_dispatch[SLOT_SND_MP3_SET_LOOP]      = tramp_snd_mp3_set_loop;
    s_dispatch[SLOT_SND_MP3_FREE]          = tramp_snd_mp3_free;

    // Appconfig
    s_dispatch[SLOT_APPCONFIG_LOAD]       = tramp_appconfig_load;
    s_dispatch[SLOT_APPCONFIG_SAVE]       = tramp_appconfig_save;
    s_dispatch[SLOT_APPCONFIG_GET]        = tramp_appconfig_get;
    s_dispatch[SLOT_APPCONFIG_SET]        = tramp_appconfig_set;
    s_dispatch[SLOT_APPCONFIG_CLEAR]      = tramp_appconfig_clear;
    s_dispatch[SLOT_APPCONFIG_RESET]      = tramp_appconfig_reset;
    s_dispatch[SLOT_APPCONFIG_GET_APP_ID] = tramp_appconfig_get_app_id;

    // Crypto (only randomBytes is implemented, rest are stubs)
    s_dispatch[SLOT_CRYPTO_RANDOM_BYTES]  = tramp_crypto_random_bytes;
    s_stub_names[SLOT_CRYPTO_SHA256]      = "crypto.sha256";
    s_stub_names[SLOT_CRYPTO_SHA1]        = "crypto.sha1";
    s_stub_names[SLOT_CRYPTO_HMAC_SHA256] = "crypto.hmacSha256";
    s_stub_names[SLOT_CRYPTO_HMAC_SHA1]   = "crypto.hmacSha1";
    s_stub_names[SLOT_CRYPTO_DERIVE_KEY]  = "crypto.deriveKey";
    s_stub_names[SLOT_CRYPTO_AES_NEW]     = "crypto.aesNew";
    s_stub_names[SLOT_CRYPTO_AES_UPDATE]  = "crypto.aesUpdate";
    s_stub_names[SLOT_CRYPTO_AES_FREE]    = "crypto.aesFree";
    s_stub_names[SLOT_CRYPTO_ECDH_X25519] = "crypto.ecdhX25519";
    s_stub_names[SLOT_CRYPTO_ECDH_P256]   = "crypto.ecdhP256";
    s_stub_names[SLOT_CRYPTO_ECDH_GET_PUBLIC_KEY] = "crypto.ecdhGetPublicKey";
    s_stub_names[SLOT_CRYPTO_ECDH_COMPUTE_SHARED] = "crypto.ecdhComputeShared";
    s_stub_names[SLOT_CRYPTO_ECDH_FREE]   = "crypto.ecdhFree";
    s_stub_names[SLOT_CRYPTO_RSA_VERIFY]  = "crypto.rsaVerify";
    s_stub_names[SLOT_CRYPTO_ECDSA_P256_VERIFY] = "crypto.ecdsaP256Verify";

    // Graphics
    s_dispatch[SLOT_GFX_LOAD]                  = tramp_gfx_load;
    s_dispatch[SLOT_GFX_NEW_BLANK]             = tramp_gfx_new_blank;
    s_dispatch[SLOT_GFX_FREE]                  = tramp_gfx_free;
    s_dispatch[SLOT_GFX_WIDTH]                 = tramp_gfx_width;
    s_dispatch[SLOT_GFX_HEIGHT]                = tramp_gfx_height;
    s_dispatch[SLOT_GFX_PIXELS]                = tramp_gfx_pixels;
    s_dispatch[SLOT_GFX_SET_TRANSPARENT_COLOR] = tramp_gfx_set_transparent_color;
    s_dispatch[SLOT_GFX_DRAW]                  = tramp_gfx_draw;
    s_dispatch[SLOT_GFX_DRAW_REGION]           = tramp_gfx_draw_region;
    s_dispatch[SLOT_GFX_DRAW_SCALED]           = tramp_gfx_draw_scaled;

    // Video
    s_dispatch[SLOT_VIDEO_NEW_PLAYER]      = tramp_video_new_player;
    s_dispatch[SLOT_VIDEO_FREE]            = tramp_video_free;
    s_dispatch[SLOT_VIDEO_LOAD]            = tramp_video_load;
    s_dispatch[SLOT_VIDEO_PLAY]            = tramp_video_play;
    s_dispatch[SLOT_VIDEO_PAUSE]           = tramp_video_pause;
    s_dispatch[SLOT_VIDEO_RESUME]          = tramp_video_resume;
    s_dispatch[SLOT_VIDEO_STOP]            = tramp_video_stop;
    s_dispatch[SLOT_VIDEO_UPDATE]          = tramp_video_update;
    s_dispatch[SLOT_VIDEO_SEEK]            = tramp_video_seek;
    s_dispatch[SLOT_VIDEO_GET_FPS]         = tramp_video_get_fps;
    s_dispatch[SLOT_VIDEO_GET_SIZE]        = tramp_video_get_size;
    s_dispatch[SLOT_VIDEO_IS_PLAYING]      = tramp_video_is_playing;
    s_dispatch[SLOT_VIDEO_IS_PAUSED]       = tramp_video_is_paused;
    s_dispatch[SLOT_VIDEO_SET_LOOP]        = tramp_video_set_loop;
    s_dispatch[SLOT_VIDEO_SET_AUTO_FLUSH]  = tramp_video_set_auto_flush;
    s_dispatch[SLOT_VIDEO_HAS_AUDIO]       = tramp_video_has_audio;
    s_dispatch[SLOT_VIDEO_SET_VOLUME]      = tramp_video_set_volume;
    s_dispatch[SLOT_VIDEO_GET_VOLUME]      = tramp_video_get_volume;
    s_dispatch[SLOT_VIDEO_SET_MUTED]       = tramp_video_set_muted;
    s_dispatch[SLOT_VIDEO_GET_MUTED]       = tramp_video_get_muted;
    s_dispatch[SLOT_VIDEO_GET_DROPPED_FRAMES] = tramp_video_get_dropped_frames;
    s_dispatch[SLOT_VIDEO_RESET_STATS]     = tramp_video_reset_stats;
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
    uint32_t terminal_count = SLOT_TERMINAL_END - SLOT_TERM_CREATE;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_TERM_CREATE, terminal_count);

    // picocalc_http_t (15 function pointers)
    uint32_t http_addr = sub_base;
    uint32_t http_count = SLOT_HTTP_END - SLOT_HTTP_NEW_CONN;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_HTTP_NEW_CONN, http_count);

    // picocalc_soundplayer_t (35 function pointers)
    uint32_t soundplayer_addr = sub_base;
    uint32_t soundplayer_count = SLOT_SOUNDPLAYER_END - SLOT_SND_SAMPLE_LOAD;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_SND_SAMPLE_LOAD, soundplayer_count);

    // picocalc_appconfig_t (7 function pointers)
    uint32_t appconfig_addr = sub_base;
    uint32_t appconfig_count = SLOT_APPCONFIG_END - SLOT_APPCONFIG_LOAD;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_APPCONFIG_LOAD, appconfig_count);

    // picocalc_crypto_t (16 function pointers)
    uint32_t crypto_addr = sub_base;
    uint32_t crypto_count = SLOT_CRYPTO_END - SLOT_CRYPTO_SHA256;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_CRYPTO_SHA256, crypto_count);

    // picocalc_graphics_t (10 function pointers)
    uint32_t graphics_addr = sub_base;
    uint32_t graphics_count = SLOT_GRAPHICS_END - SLOT_GFX_LOAD;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_GFX_LOAD, graphics_count);

    // picocalc_video_t (22 function pointers)
    uint32_t video_addr = sub_base;
    uint32_t video_count = SLOT_VIDEO_END - SLOT_VIDEO_NEW_PLAYER;
    sub_base = write_func_table(uc, sub_base, tramp_base, SLOT_VIDEO_NEW_PLAYER, video_count);

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
