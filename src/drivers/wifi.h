#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "../os/os.h"   // wifi_status_t
#include "http.h"       // http_conn_t (for conn_req_t)

// =============================================================================
// WiFi Driver — CYW43 on Pimoroni Pico Plus 2W
//
// SPI1 is shared between the LCD and the CYW43 WiFi chip. All CYW43
// operations are serialised behind display_spi_lock() / display_spi_unlock()
// to prevent bus conflicts with LCD DMA transfers.
//
// Connection is non-blocking: call wifi_connect(), then poll wifi_get_status()
// or let the OS Lua hook drive wifi_poll() automatically in the background.
//
// Compile guard: WIFI_ENABLED=1 is defined by CMakeLists when the board has
// a CYW43 chip. All functions are safe no-ops when WIFI_ENABLED is absent.
//
// Thread model: Core 1 is the sole owner of the Mongoose manager. Core 0
// never calls mg_* functions directly. Instead it pushes conn_req_t entries
// to a spinlock-guarded ring buffer; Core 1's wifi_poll() drains them before
// each mg_mgr_poll() call.
// =============================================================================

// Initialise CYW43 hardware and enable station mode. Call once during boot
// after sdcard / config are initialised. Sets wifi_is_available() if hardware
// is found. If config holds "wifi_ssid" / "wifi_pass", starts auto-connect.
void wifi_init(void);

// Returns true if CYW43 hardware was found and initialised successfully.
bool wifi_is_available(void);

// Begin connecting to a WiFi network (non-blocking, WPA/WPA2).
// s_status transitions to WIFI_STATUS_CONNECTING immediately; check
// wifi_get_status() for CONNECTED or FAILED.
void wifi_connect(const char *ssid, const char *password);

// Disconnect from the current network.
void wifi_disconnect(void);

// Current connection state (WIFI_STATUS_*).
wifi_status_t wifi_get_status(void);

// Null-terminated IP address string (e.g. "192.168.1.42"), or NULL if not
// connected.
const char *wifi_get_ip(void);

// SSID of the current or pending connection, or NULL if fully disconnected.
const char *wifi_get_ssid(void);

// Drive the CYW43 lwip-poll state machine and update connection status.
// Must be called regularly from Core 1. No-op when WiFi is not available.
void wifi_poll(void);

// Set whether an app with HTTP requirement is running. When true, WiFi will
// not automatically disconnect after SNTP time sync.
void wifi_set_http_required(bool required);
bool wifi_get_http_required(void);

// ── Core 0 → Core 1 request queue ────────────────────────────────────────────
//
// Core 0 (Lua main loop) must never call mg_* functions directly. Instead it
// populates the relevant http_conn_t fields and pushes a conn_req_t to this
// queue. Core 1's drain_requests() (called from wifi_poll) executes the
// actual Mongoose operations.

typedef enum {
    CONN_REQ_HTTP_START,      // new TCP conn (or reuse keep-alive) + send request
    CONN_REQ_HTTP_CLOSE,      // mark nc->is_closing = 1
    CONN_REQ_WIFI_CONNECT,    // mg_wifi_connect()
    CONN_REQ_WIFI_DISCONNECT, // mg_wifi_disconnect()
} conn_req_type_t;

typedef struct {
    conn_req_type_t  type;
    http_conn_t     *conn;       // NULL for wifi ops
    char             ssid[64];   // used by CONN_REQ_WIFI_CONNECT
    char             pass[64];
} conn_req_t;

// Push a request from Core 0 to the Core 1 queue.
// Returns false (and does nothing) if the 8-slot queue is full.
bool wifi_req_push(const conn_req_t *req);
