#pragma once

#include <stdint.h>
#include <stdbool.h>

// Forward declare terminal_t (defined in terminal.h)
typedef struct terminal terminal_t;

typedef struct {
    terminal_t* (*create)(int cols, int rows, int scrollback_lines);
    void (*free)(terminal_t* term);
    void (*clear)(terminal_t* term);
    void (*write)(terminal_t* term, const char* str);
    void (*putChar)(terminal_t* term, char c);
    void (*setCursor)(terminal_t* term, int x, int y);
    void (*getCursor)(terminal_t* term, int* out_x, int* out_y);
    void (*setColors)(terminal_t* term, uint16_t fg, uint16_t bg);
    void (*getColors)(terminal_t* term, uint16_t* out_fg, uint16_t* out_bg);
    void (*scroll)(terminal_t* term, int lines);
    void (*render)(terminal_t* term);
    void (*renderDirty)(terminal_t* term);
    int (*getCols)(terminal_t* term);
    int (*getRows)(terminal_t* term);
    void (*setCursorVisible)(bool visible);
    void (*setCursorBlink)(bool blink);
    void (*markAllDirty)(terminal_t* term);
    bool (*isFullDirty)(terminal_t* term);
    void (*getDirtyRange)(terminal_t* term, int* out_first, int* out_last);
    int (*getScrollbackCount)(terminal_t* term);
    void (*setScrollbackOffset)(terminal_t* term, int offset);
    int (*getScrollbackOffset)(terminal_t* term);
    void (*getScrollbackLine)(terminal_t* term, int line, uint16_t* out_cells);
    void (*getScrollbackLineColors)(terminal_t* term, int line, uint16_t* out_fg, uint16_t* out_bg);
} picocalc_terminal_t;

// =============================================================================
// PicoOS API
//
// This is the central interface between the OS and apps. The OS owns all hardware;
// apps borrow it through here.
//
// In Lua, this is exposed as the `picocalc` global module.
// In C (future), a pointer to this struct is passed to the app entry point.
// =============================================================================

// --- Input ------------------------------------------------------------------

// Button bitmask values (keyboard + d-pad)
#define BTN_UP        (1 << 0)
#define BTN_DOWN      (1 << 1)
#define BTN_LEFT      (1 << 2)
#define BTN_RIGHT     (1 << 3)
#define BTN_ENTER     (1 << 4)    // Enter key
#define BTN_ESC       (1 << 5)    // Escape key
#define BTN_MENU      (1 << 6)    // System menu trigger (F10 key)
#define BTN_F1        (1 << 7)
#define BTN_F2        (1 << 8)
#define BTN_F3        (1 << 9)
#define BTN_F4        (1 << 10)
#define BTN_F5        (1 << 11)
#define BTN_F6        (1 << 12)
#define BTN_F7        (1 << 13)
#define BTN_F8        (1 << 14)
#define BTN_F9        (1 << 15)
#define BTN_BACKSPACE (1 << 16)   // Backspace key
#define BTN_TAB       (1 << 17)   // Tab key
#define BTN_DEL       (1 << 18)   // Delete key (Fn+Backspace typically)
#define BTN_SHIFT     (1 << 19)   // Shift modifier
#define BTN_CTRL      (1 << 20)   // Ctrl modifier
#define BTN_ALT       (1 << 21)   // Alt modifier
#define BTN_FN        (1 << 22)   // Fn/Symbol modifier

typedef struct {
    // Returns current bitmask of held buttons (BTN_* flags)
    uint32_t (*getButtons)(void);
    // Returns bitmask of buttons pressed THIS frame (edge detect, not held)
    uint32_t (*getButtonsPressed)(void);
    // Returns bitmask of buttons released THIS frame
    uint32_t (*getButtonsReleased)(void);
    // Returns the last ASCII character typed (0 if none this frame)
    // Includes full keyboard; use this for text input
    char (*getChar)(void);
} picocalc_input_t;

// --- Display ----------------------------------------------------------------

typedef struct {
    void (*clear)(uint16_t color_rgb565);
    void (*setPixel)(int x, int y, uint16_t color);
    void (*fillRect)(int x, int y, int w, int h, uint16_t color);
    void (*drawRect)(int x, int y, int w, int h, uint16_t color);
    void (*drawLine)(int x0, int y0, int x1, int y1, uint16_t color);
    void (*drawCircle)(int cx, int cy, int r, uint16_t color);
    void (*fillCircle)(int cx, int cy, int r, uint16_t color);
    // Draw a null-terminated string. Returns pixel width of drawn text.
    int  (*drawText)(int x, int y, const char *text, uint16_t fg, uint16_t bg);
    // Flush the internal framebuffer to the LCD (call once per frame)
    void (*flush)(void);
    // Returns display width/height
    int  (*getWidth)(void);
    int  (*getHeight)(void);
    // Set display brightness 0-255 (controls backlight PWM)
    void (*setBrightness)(uint8_t brightness);
    // Draw an integer-scaled image (nearest-neighbor, no transparency).
    // Input data in host byte order (RGB565). Ideal for emulator blits.
    void (*drawImageNN)(int x, int y, const uint16_t *data,
                        int src_w, int src_h, int scale);
    // Flush only rows y0..y1 (inclusive, full width) from the back buffer.
    // Does NOT swap buffers. Useful for partial screen updates.
    void (*flushRows)(int y0, int y1);
    // Flush rows y0..y1 (inclusive) with buffer swap. Like flush() but only
    // transfers the specified row range. Useful for emulators with letterboxing.
    void (*flushRegion)(int y0, int y1);
    // Get writable pointer to the current back buffer (320x320 RGB565,
    // big-endian / byte-swapped). For direct framebuffer writes.
    uint16_t* (*getBackBuffer)(void);
} picocalc_display_t;

// --- Filesystem (SD card) ---------------------------------------------------

typedef void* pcfile_t;   // opaque file handle

typedef struct {
    // Open a file. mode: "r", "w", "a", "rb", "wb" etc.
    pcfile_t (*open)(const char *path, const char *mode);
    int      (*read)(pcfile_t f, void *buf, int len);
    int      (*write)(pcfile_t f, const void *buf, int len);
    void     (*close)(pcfile_t f);
    bool     (*exists)(const char *path);
    int      (*size)(const char *path);
    int      (*fsize)(pcfile_t f);
    bool     (*seek)(pcfile_t f, uint32_t offset);
    uint32_t (*tell)(pcfile_t f);
    // List directory. Calls callback for each entry. Returns entry count.
    int      (*listDir)(const char *path,
                        void (*callback)(const char *name, bool is_dir,
                                         uint32_t size, void *user),
                        void *user);
} picocalc_fs_t;

// --- System -----------------------------------------------------------------

typedef struct {
    // Milliseconds since boot
    uint32_t (*getTimeMs)(void);
    // Microseconds since boot (64-bit, for high-precision timing)
    uint64_t (*getTimeUs)(void);
    // Trigger a system reboot
    void     (*reboot)(void);
    // Battery level 0-100 (from STM32 via I2C). -1 = unknown/USB powered.
    int      (*getBatteryPercent)(void);
    // True if connected to USB power
    bool     (*isUSBPowered)(void);
    // Add an item to the system menu overlay (max 4 items per app)
    // callback is called when the item is selected in the menu
    void     (*addMenuItem)(const char *label, void (*callback)(void *user), void *user);
    // Clear all app-registered menu items (called automatically on app exit)
    void     (*clearMenuItems)(void);
    // Log a message to UART serial debug output
    void     (*log)(const char *fmt, ...);
    // Single OS tick for native apps: polls keyboard + fires any pending
    // C HTTP callbacks.  Also checks for the Sym (Menu) key and shows the
    // system menu overlay automatically.  Call in your main loop.
    void     (*poll)(void);
    // Returns true (once) after the user selects "Exit App" from the system
    // menu.  Native apps should check this each frame and return from
    // picos_main() when it fires.
    bool     (*shouldExit)(void);
    // Register a callback to be called on Core 1 every 5ms, alongside
    // audio updates.  Used by native apps (e.g. DOOM) to offload audio
    // mixing from Core 0.  Pass NULL to deregister.
    void     (*setAudioCallback)(void (*cb)(void));
} picocalc_sys_t;

// --- Audio ------------------------------------------------------------------

typedef struct {
    // Play a square wave tone at freq Hz for duration_ms milliseconds.
    // duration_ms = 0 plays indefinitely until stopTone() is called.
    void (*playTone)(uint32_t freq_hz, uint32_t duration_ms);
    void (*stopTone)(void);
    // Master volume 0-100
    void (*setVolume)(uint8_t volume);
    // PCM sample streaming. Samples are stereo interleaved int16_t (L,R,L,R).
    // count = number of stereo frames (each frame = 2 int16_t values).
    void (*startStream)(uint32_t sample_rate);
    void (*stopStream)(void);
    void (*pushSamples)(const int16_t *samples, int count);
} picocalc_audio_t;

// --- WiFi (Pico 2W only, shares SPI1 with LCD) ------------------------------
// The OS manages the SPI bus arbitration. Apps must not call these
// while the display is being flushed. The OS handles this automatically.

typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED,
} wifi_status_t;

typedef struct {
    // Connect to a WiFi network. Non-blocking: check status with getStatus().
    void (*connect)(const char *ssid, const char *password);
    void (*disconnect)(void);
    wifi_status_t (*getStatus)(void);
    // Returns current IP as a string, or NULL if not connected
    const char *  (*getIP)(void);
    // Returns SSID of current connection, or NULL
    const char *  (*getSSID)(void);
    // True if WiFi hardware is present (Pico 2W vs standard Pico 2)
    bool          (*isAvailable)(void);
} picocalc_wifi_t;

// --- TCP Sockets ------------------------------------------------------------

typedef void* pctcp_t;   // opaque TCP connection handle

typedef enum {
    TCP_CB_CONNECT  = (1 << 0),
    TCP_CB_READ     = (1 << 1),
    TCP_CB_WRITE    = (1 << 2),
    TCP_CB_CLOSED   = (1 << 3),
    TCP_CB_FAILED   = (1 << 4),
} tcp_event_t;

typedef struct {
    // Open a TCP connection to host:port. Non-blocking.
    pctcp_t (*connect)(const char *host, uint16_t port, bool use_ssl);
    // Write data to the connection. Returns bytes sent or <0 on error.
    int     (*write)(pctcp_t c, const void *buf, int len);
    // Read data from the connection. Returns bytes read or 0 if none available.
    int     (*read)(pctcp_t c, void *buf, int len);
    // Close the connection.
    void    (*close)(pctcp_t c);
    // Returns number of bytes available for reading.
    int     (*available)(pctcp_t c);
    // Returns the last error string, or NULL.
    const char * (*getError)(pctcp_t c);
    // Returns bitmask of pending events (TCP_CB_*).
    uint32_t (*getEvents)(pctcp_t c);
} picocalc_tcp_t;

// --- UI Widgets -------------------------------------------------------------

typedef struct {
    // Modal text input with title bar. Returns true on Enter, false on Esc.
    bool (*textInput)(const char *title, const char *prompt,
                      const char *initial, char *out, int out_len);
    // Simpler text input (no title bar). Returns true on Enter, false on Esc.
    bool (*textInputSimple)(const char *prompt, const char *default_val,
                            char *out_buf, int out_len);
    // Yes/no confirmation dialog. Returns true on Enter/Y, false on Esc/N.
    bool (*confirm)(const char *message);
} picocalc_ui_t;

// --- PSRAM Benchmark ---------------------------------------------------------

typedef struct {
    // Check if PIO PSRAM (mainboard) is available
    bool (*pioAvailable)(void);
    // Check if PIO PSRAM bulk driver is available
    bool (*pioBulkAvailable)(void);
    // Read from PIO PSRAM (original driver, 27B chunks)
    void (*pioRead)(uint32_t addr, uint8_t *dst, uint32_t len);
    // Write to PIO PSRAM (original driver, 27B chunks)
    void (*pioWrite)(uint32_t addr, const uint8_t *src, uint32_t len);
    // Read from PIO PSRAM (bulk driver, 8KB chunks)
    void (*pioBulkRead)(uint32_t addr, uint8_t *dst, uint32_t len);
    // Write to PIO PSRAM (bulk driver, 8KB chunks)
    void (*pioBulkWrite)(uint32_t addr, const uint8_t *src, uint32_t len);
    // Allocate memory in QMI PSRAM (via umm_malloc)
    void *(*qmiAlloc)(uint32_t size);
    // Free memory from QMI PSRAM (via umm_free)
    void (*qmiFree)(void *ptr);
} picocalc_psram_t;

// --- Performance -----------------------------------------------------------

typedef struct {
    void (*beginFrame)(void);
    void (*endFrame)(void);
    int  (*getFPS)(void);
    uint32_t (*getFrameTime)(void);
    void (*drawFPS)(int x, int y);
    void (*setTargetFPS)(uint32_t fps);
} picocalc_perf_t;

// --- The complete OS API struct ---------------------------------------------
// This is what gets passed to every Lua environment and future C app loaders.

typedef struct PicoCalcAPI {
    const picocalc_input_t   *input;
    const picocalc_display_t *display;
    const picocalc_fs_t      *fs;
    const picocalc_sys_t     *sys;
    const picocalc_audio_t   *audio;
    const picocalc_wifi_t    *wifi;
    const picocalc_tcp_t     *tcp;
    const picocalc_ui_t      *ui;
    const picocalc_psram_t   *psram;
    const picocalc_perf_t    *perf;
    const picocalc_terminal_t *terminal;
} PicoCalcAPI;

// The global API instance, populated during os_init()
extern PicoCalcAPI g_api;

// Optional audio callback for native apps that need Core 1 mixing.
// Set by the native app at startup, cleared on exit. Called every 5ms
// from core1_entry() alongside mp3_player_update()/fileplayer_update().
extern void (*g_native_audio_callback)(void);
