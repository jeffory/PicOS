#pragma once

#include <stdint.h>
#include <stdbool.h>

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
#define BTN_UP      (1 << 0)
#define BTN_DOWN    (1 << 1)
#define BTN_LEFT    (1 << 2)
#define BTN_RIGHT   (1 << 3)
#define BTN_ENTER   (1 << 4)    // Enter key
#define BTN_ESC     (1 << 5)    // Escape key
#define BTN_MENU    (1 << 6)    // System menu trigger

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
    // Draw a null-terminated string. Returns pixel width of drawn text.
    int  (*drawText)(int x, int y, const char *text, uint16_t fg, uint16_t bg);
    // Flush the internal framebuffer to the LCD (call once per frame)
    void (*flush)(void);
    // Returns display width/height
    int  (*getWidth)(void);
    int  (*getHeight)(void);
    // Set display brightness 0-255 (controls backlight PWM)
    void (*setBrightness)(uint8_t brightness);
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
    // List directory. Calls callback for each entry. Returns entry count.
    int      (*listDir)(const char *path,
                        void (*callback)(const char *name, bool is_dir, void *user),
                        void *user);
} picocalc_fs_t;

// --- System -----------------------------------------------------------------

typedef struct {
    // Milliseconds since boot
    uint32_t (*getTimeMs)(void);
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
} picocalc_sys_t;

// --- Audio ------------------------------------------------------------------

typedef struct {
    // Play a square wave tone at freq Hz for duration_ms milliseconds.
    // duration_ms = 0 plays indefinitely until stopTone() is called.
    void (*playTone)(uint32_t freq_hz, uint32_t duration_ms);
    void (*stopTone)(void);
    // Master volume 0-100
    void (*setVolume)(uint8_t volume);
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

// --- The complete OS API struct ---------------------------------------------
// This is what gets passed to every Lua environment and future C app loaders.

typedef struct {
    const picocalc_input_t   *input;
    const picocalc_display_t *display;
    const picocalc_fs_t      *fs;
    const picocalc_sys_t     *sys;
    const picocalc_audio_t   *audio;
    const picocalc_wifi_t    *wifi;
} PicoCalcAPI;

// The global API instance, populated during os_init()
extern PicoCalcAPI g_api;
