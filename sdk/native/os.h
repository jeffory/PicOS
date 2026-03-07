// This file is a copy of src/os/os.h for use by native app developers.
// It defines the complete PicoCalcAPI type that is passed to picos_main().
// Include this alongside app_abi.h in your app sources.
//
// DO NOT modify this file — it must stay in sync with the OS source.

#pragma once

#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// PicoOS API
// =============================================================================

// --- Input -------------------------------------------------------------------

#define BTN_UP        (1 << 0)
#define BTN_DOWN      (1 << 1)
#define BTN_LEFT      (1 << 2)
#define BTN_RIGHT     (1 << 3)
#define BTN_ENTER     (1 << 4)
#define BTN_ESC       (1 << 5)
#define BTN_MENU      (1 << 6)
#define BTN_F1        (1 << 7)
#define BTN_F2        (1 << 8)
#define BTN_F3        (1 << 9)
#define BTN_F4        (1 << 10)
#define BTN_F5        (1 << 11)
#define BTN_F6        (1 << 12)
#define BTN_F7        (1 << 13)
#define BTN_F8        (1 << 14)
#define BTN_F9        (1 << 15)
#define BTN_BACKSPACE (1 << 16)
#define BTN_TAB       (1 << 17)
#define BTN_DEL       (1 << 18)
#define BTN_SHIFT     (1 << 19)
#define BTN_CTRL      (1 << 20)
#define BTN_ALT       (1 << 21)
#define BTN_FN        (1 << 22)

typedef struct {
    uint32_t (*getButtons)(void);
    uint32_t (*getButtonsPressed)(void);
    uint32_t (*getButtonsReleased)(void);
    char     (*getChar)(void);
} picocalc_input_t;

// --- Display -----------------------------------------------------------------

typedef struct {
    void (*clear)(uint16_t color_rgb565);
    void (*setPixel)(int x, int y, uint16_t color);
    void (*fillRect)(int x, int y, int w, int h, uint16_t color);
    void (*drawRect)(int x, int y, int w, int h, uint16_t color);
    void (*drawLine)(int x0, int y0, int x1, int y1, uint16_t color);
    int  (*drawText)(int x, int y, const char *text, uint16_t fg, uint16_t bg);
    void (*flush)(void);
    int  (*getWidth)(void);
    int  (*getHeight)(void);
    void (*setBrightness)(uint8_t brightness);
    void (*drawImageNN)(int x, int y, const uint16_t *data,
                        int src_w, int src_h, int scale);
    void (*flushRows)(int y0, int y1);
} picocalc_display_t;

// --- Filesystem --------------------------------------------------------------

typedef void *pcfile_t;

typedef struct {
    pcfile_t (*open)(const char *path, const char *mode);
    int      (*read)(pcfile_t f, void *buf, int len);
    int      (*write)(pcfile_t f, const void *buf, int len);
    void     (*close)(pcfile_t f);
    bool     (*exists)(const char *path);
    int      (*size)(const char *path);
    int      (*listDir)(const char *path,
                        void (*callback)(const char *name, bool is_dir, void *user),
                        void *user);
} picocalc_fs_t;

// --- System ------------------------------------------------------------------

typedef struct {
    uint32_t (*getTimeMs)(void);
    void     (*reboot)(void);
    int      (*getBatteryPercent)(void);
    bool     (*isUSBPowered)(void);
    void     (*addMenuItem)(const char *label, void (*callback)(void *user), void *user);
    void     (*clearMenuItems)(void);
    void     (*log)(const char *fmt, ...);
    // OS tick for native apps: polls keyboard + fires pending C HTTP callbacks.
    // Also checks for the Sym (Menu) key and shows the system menu overlay.
    // Call this in your main loop (e.g. once per frame).
    void     (*poll)(void);
    // Returns true (once) after the user selects "Exit App" from the system
    // menu.  Check this each frame and return from picos_main() when it fires.
    bool     (*shouldExit)(void);
} picocalc_sys_t;

// --- Audio -------------------------------------------------------------------

typedef struct {
    void (*playTone)(uint32_t freq_hz, uint32_t duration_ms);
    void (*stopTone)(void);
    void (*setVolume)(uint8_t volume);
    void (*startStream)(uint32_t sample_rate);
    void (*stopStream)(void);
    void (*pushSamples)(const int16_t *samples, int count);
} picocalc_audio_t;

// --- WiFi --------------------------------------------------------------------

typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED,
} wifi_status_t;

typedef struct {
    void          (*connect)(const char *ssid, const char *password);
    void          (*disconnect)(void);
    wifi_status_t (*getStatus)(void);
    const char *  (*getIP)(void);
    const char *  (*getSSID)(void);
    bool          (*isAvailable)(void);
} picocalc_wifi_t;

// --- TCP Sockets -------------------------------------------------------------

typedef void *pctcp_t;

typedef enum {
    TCP_CB_CONNECT  = (1 << 0),
    TCP_CB_READ     = (1 << 1),
    TCP_CB_WRITE    = (1 << 2),
    TCP_CB_CLOSED   = (1 << 3),
    TCP_CB_FAILED   = (1 << 4),
} tcp_event_t;

typedef struct {
    pctcp_t      (*connect)(const char *host, uint16_t port, bool use_ssl);
    int          (*write)(pctcp_t c, const void *buf, int len);
    int          (*read)(pctcp_t c, void *buf, int len);
    void         (*close)(pctcp_t c);
    int          (*available)(pctcp_t c);
    const char * (*getError)(pctcp_t c);
    uint32_t     (*getEvents)(pctcp_t c);
} picocalc_tcp_t;

// --- UI Widgets --------------------------------------------------------------

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

// --- Complete OS API struct --------------------------------------------------

typedef struct PicoCalcAPI {
    const picocalc_input_t   *input;
    const picocalc_display_t *display;
    const picocalc_fs_t      *fs;
    const picocalc_sys_t     *sys;
    const picocalc_audio_t   *audio;
    const picocalc_wifi_t    *wifi;
    const picocalc_tcp_t     *tcp;
    const picocalc_ui_t      *ui;
} PicoCalcAPI;
