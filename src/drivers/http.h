#pragma once

// =============================================================================
// HTTP/1.1 client over lwIP TCP + DNS for PicOS
//
// Non-blocking, poll-driven. All network I/O happens inside cyw43_arch_poll()
// (called from wifi_poll()). lwIP callbacks set pending flags; the Lua bridge
// reads and fires those callbacks from menu_lua_hook after wifi_poll() returns.
//
// Compile guard: WIFI_ENABLED=1 is defined by CMakeLists for WiFi boards.
// All functions are safe no-ops when WIFI_ENABLED is absent.
// =============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef WIFI_ENABLED
// No-op for now, Mongoose handles its own includes
#endif

// ── Limits ────────────────────────────────────────────────────────────────────

#define HTTP_MAX_CONNECTIONS   8      // Simultaneous connections
#define HTTP_RECV_BUF_DEFAULT  4096   // Default receive ring buffer
#define HTTP_RECV_BUF_MAX      32768  // Max allowed by setReadBufferSize
#define HTTP_HEADER_BUF_MAX    2048   // Raw response header block
#define HTTP_MAX_HDR_ENTRIES   24     // Max parsed header fields
#define HTTP_SERVER_MAX        128    // Hostname buffer
#define HTTP_ERR_MAX           128    // Error string buffer

// ── Pending callback bitmask ──────────────────────────────────────────────────
// Set by lwIP callbacks, consumed by lua_bridge.c in menu_lua_hook.

#define HTTP_CB_REQUEST   (1u << 0)   // Data arrived (fires setRequestCallback)
#define HTTP_CB_HEADERS   (1u << 1)   // Headers parsed (fires setHeadersReadCallback)
#define HTTP_CB_COMPLETE  (1u << 2)   // Body done (fires setRequestCompleteCallback)
#define HTTP_CB_CLOSED    (1u << 3)   // Connection closed (fires setConnectionClosedCallback)
#define HTTP_CB_FAILED    (1u << 4)   // Error occurred (fires setConnectionClosedCallback)

// ── State ─────────────────────────────────────────────────────────────────────

typedef enum {
    HTTP_STATE_IDLE = 0,
    HTTP_STATE_DNS,         // DNS resolution in progress
    HTTP_STATE_CONNECTING,  // TCP connect in progress
    HTTP_STATE_SENDING,     // Sending HTTP request
    HTTP_STATE_HEADERS,     // Receiving and parsing response headers
    HTTP_STATE_BODY,        // Receiving response body
    HTTP_STATE_DONE,        // Request complete
    HTTP_STATE_FAILED,      // Error
} http_state_t;

// ── Connection struct ─────────────────────────────────────────────────────────

typedef struct {
    bool         in_use;
    http_state_t state;

    // Configuration (set by lua_bridge before issuing a request)
    char     server[HTTP_SERVER_MAX];
    char     path[256];
    char     method[8];
    char    *extra_hdrs;
    uint16_t port;
    bool     use_ssl;
    bool     keep_alive;
    int32_t  range_from;         // -1 = not set
    int32_t  range_to;           // -1 = not set
    uint32_t connect_timeout_ms;
    uint32_t read_timeout_ms;

    // Error string (non-empty on failure)
    char err[HTTP_ERR_MAX];

    // Pending Lua-callback flags (HTTP_CB_*), set in lwIP callbacks
    uint8_t pending;

    // Response metadata
    int      status_code;
    char    *hdr_buf;        // malloc'd HTTP_HEADER_BUF_MAX bytes
    size_t   hdr_len;        // bytes written into hdr_buf
    bool     headers_done;
    int32_t  content_length; // -1 = unknown (chunked / no Content-Length)
    uint32_t body_received;

    // Parsed header fields (pointers into hdr_buf, null-terminated in place)
    const char *hdr_keys[HTTP_MAX_HDR_ENTRIES];
    const char *hdr_vals[HTTP_MAX_HDR_ENTRIES];
    int         hdr_count;

    // Receive ring buffer (body data)
    uint8_t *rx_buf;
    uint32_t rx_cap;   // allocated size
    uint32_t rx_head;  // write index
    uint32_t rx_tail;  // read index
    uint32_t rx_count; // bytes available

    // Transmit buffer (HTTP request, heap-alloc'd, freed after sent)
    char    *tx_buf;
    uint32_t tx_len;
    uint32_t tx_sent;

    // Deadline timestamps (ms since boot)
    uint32_t deadline_connect;
    uint32_t deadline_read;

    // Opaque pointer back to Lua userdata (set by lua_bridge.c)
    void *lua_ud;

    // Internal mongoose connection pointer
    void *pcb;
} http_conn_t;

// ── Public API ────────────────────────────────────────────────────────────────

// Initialize the connection pool. Call once at boot after wifi_init().
void http_init(void);

// Close all active connections (clears lua_ud pointers). Call before
// launching a new Lua app so stale callbacks cannot fire.
// on_free is called for each active connection to allow Lua-side cleanup.
void http_close_all(void (*on_free)(void *lua_ud));

// Allocate a connection from the static pool. Returns NULL if pool is full.
// Caller must set conn->server, conn->port before issuing a request.
http_conn_t *http_alloc(void);

// Release a connection: closes TCP, frees buffers, marks slot free.
void http_free(http_conn_t *c);

// Gracefully close the TCP connection without freeing the slot.
// Resets state to IDLE so the slot can be reused for another request.
void http_close(http_conn_t *c);

// Resize the receive ring buffer. Must be called before issuing a request.
// Clamps to HTTP_RECV_BUF_MAX. Returns false on OOM.
bool http_set_recv_buf(http_conn_t *c, uint32_t bytes);

// Issue an HTTP GET request. extra_hdr: optional "Key: Value\r\n..." string.
// Returns true if DNS lookup/connection was successfully started.
bool http_get(http_conn_t *c, const char *path, const char *extra_hdr);

// Issue an HTTP POST request with a body. extra_hdr optional.
bool http_post(http_conn_t *c, const char *path, const char *extra_hdr,
               const char *body, size_t body_len);

// Read up to `len` bytes from the receive ring buffer into `out`.
// Returns number of bytes actually copied (0 if none available).
uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len);

// Return bytes currently available in the receive ring buffer.
uint32_t http_bytes_available(http_conn_t *c);

// Return a pool slot by index (0 .. HTTP_MAX_CONNECTIONS-1).
// Returns NULL if the slot is not in use.
http_conn_t *http_get_conn(int idx);

// Atomically read and clear the pending callback bitmask.
uint8_t http_take_pending(http_conn_t *c);

// Poll Mongoose for network events.
void http_poll(void);

// Fire any pending C-language (non-Lua) HTTP callbacks.
// Called from Core 1 after wifi_poll() so native apps get network events
// without needing a Lua-style opcode hook.
void http_fire_c_pending(void);
