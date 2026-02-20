#include "http.h"
#include "display.h"    // display_spi_lock/unlock (needed for cyw43 guard)
#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

#ifdef WIFI_ENABLED
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#endif

// ── Static pool ───────────────────────────────────────────────────────────────

static http_conn_t s_conns[HTTP_MAX_CONNECTIONS];

// ── Forward declarations ──────────────────────────────────────────────────────

#ifdef WIFI_ENABLED
static void  dns_found_cb(const char *name, const ip_addr_t *addr, void *arg);
static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err);
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len);
static void  tcp_err_cb(void *arg, err_t err);
static err_t tcp_poll_cb(void *arg, struct tcp_pcb *pcb);
static err_t try_send(http_conn_t *c, struct tcp_pcb *pcb);
static bool  setup_tcp(http_conn_t *c, const ip_addr_t *addr);
#endif

// ── Internal helpers ──────────────────────────────────────────────────────────

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void conn_fail(http_conn_t *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof(c->err), fmt, ap);
    va_end(ap);
    c->state   = HTTP_STATE_FAILED;
    c->pending |= HTTP_CB_FAILED | HTTP_CB_CLOSED;
}

// Case-insensitive string equality
static bool str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

// ── Ring-buffer helpers ───────────────────────────────────────────────────────

static void rx_write(http_conn_t *c, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (c->rx_count >= c->rx_cap) break;  // drop on overflow
        c->rx_buf[c->rx_head] = data[i];
        c->rx_head = (c->rx_head + 1) % c->rx_cap;
        c->rx_count++;
    }
}

// ── HTTP response header parsing ──────────────────────────────────────────────

// Extract numeric status code from "HTTP/1.x NNN reason"
static int parse_status_line(const char *line) {
    const char *p = strchr(line, ' ');
    return p ? (int)strtol(p + 1, NULL, 10) : 0;
}

// Parse hdr_buf in-place: null-terminate keys and values, populate hdr_keys/vals.
// Called once when \r\n\r\n is detected.
static void parse_headers(http_conn_t *c) {
    c->hdr_count   = 0;
    c->status_code = 0;
    c->content_length = -1;

    char *p = c->hdr_buf;

    // Parse status line (first line)
    char *eol = strstr(p, "\r\n");
    if (!eol) return;
    c->status_code = parse_status_line(p);
    p = eol + 2;

    // Parse header fields
    while (*p && c->hdr_count < HTTP_MAX_HDR_ENTRIES) {
        if (p[0] == '\r' && p[1] == '\n') break;  // empty line = end of headers

        eol = strstr(p, "\r\n");
        if (!eol) break;

        char *colon = (char *)memchr(p, ':', (size_t)(eol - p));
        if (colon) {
            // Trim trailing spaces from key and null-terminate at kend
            char *kend = colon;
            while (kend > p && kend[-1] == ' ') kend--;
            *kend = '\0';  // null-terminates key; if kend==colon, replaces ':'

            // Skip leading spaces from value, null-terminate at eol
            char *val = colon + 1;
            while (*val == ' ') val++;
            *eol = '\0';  // null-terminates value

            c->hdr_keys[c->hdr_count] = p;
            c->hdr_vals[c->hdr_count] = val;

            // Grab Content-Length while we're here
            if (str_ieq(p, "content-length")) {
                c->content_length = (int32_t)strtol(val, NULL, 10);
            }

            c->hdr_count++;
        }

        p = eol + 2;
    }
}

// ── Request builder ───────────────────────────────────────────────────────────

static char *build_request(http_conn_t *c, const char *method, const char *path,
                            const char *extra_hdr, const char *body, size_t body_len,
                            uint32_t *out_len) {
    size_t max = 512
                 + (path      ? strlen(path)      : 0)
                 + (extra_hdr ? strlen(extra_hdr) : 0)
                 + body_len + 64;

    char *buf = malloc(max);
    if (!buf) return NULL;

    int n = 0;
    n += snprintf(buf + n, max - n, "%s %s HTTP/1.1\r\n", method, path);
    n += snprintf(buf + n, max - n, "Host: %s\r\n", c->server);
    n += snprintf(buf + n, max - n, "Connection: %s\r\n",
                  c->keep_alive ? "keep-alive" : "close");

    if (c->range_from >= 0) {
        if (c->range_to >= 0)
            n += snprintf(buf + n, max - n, "Range: bytes=%d-%d\r\n",
                          (int)c->range_from, (int)c->range_to);
        else
            n += snprintf(buf + n, max - n, "Range: bytes=%d-\r\n",
                          (int)c->range_from);
    }

    if (body_len > 0)
        n += snprintf(buf + n, max - n, "Content-Length: %u\r\n", (unsigned)body_len);

    if (extra_hdr && *extra_hdr) {
        n += snprintf(buf + n, max - n, "%s", extra_hdr);
        // Ensure extra headers end with \r\n
        if (n >= 2 && (buf[n-2] != '\r' || buf[n-1] != '\n'))
            n += snprintf(buf + n, max - n, "\r\n");
    }

    n += snprintf(buf + n, max - n, "\r\n");

    // Append body (may contain null bytes, so use memcpy)
    if (body && body_len > 0) {
        if ((size_t)n + body_len > max) {
            char *nb = realloc(buf, (size_t)n + body_len);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        memcpy(buf + n, body, body_len);
        n += (int)body_len;
    }

    *out_len = (uint32_t)n;
    return buf;
}

// ── Receive data processor ────────────────────────────────────────────────────

// Called from tcp_recv_cb for each chunk of incoming data.
static void process_recv(http_conn_t *c, const uint8_t *data, uint32_t len) {
    uint32_t i = 0;

    // Phase 1: accumulate into hdr_buf until we see \r\n\r\n
    while (i < len && !c->headers_done) {
        if (c->hdr_len >= HTTP_HEADER_BUF_MAX - 1) {
            conn_fail(c, "Response headers exceed %d bytes", HTTP_HEADER_BUF_MAX);
            return;
        }
        c->hdr_buf[c->hdr_len++] = (char)data[i++];
        c->hdr_buf[c->hdr_len]   = '\0';

        // Check for header terminator \r\n\r\n
        if (c->hdr_len >= 4) {
            const char *end = c->hdr_buf + c->hdr_len;
            if (end[-4]=='\r' && end[-3]=='\n' && end[-2]=='\r' && end[-1]=='\n') {
                c->headers_done = true;
                c->state        = HTTP_STATE_BODY;
                parse_headers(c);
                c->pending |= HTTP_CB_HEADERS | HTTP_CB_REQUEST;
            }
        }
    }

    // Phase 2: body data — write into receive ring buffer
    if (c->headers_done && i < len) {
        uint32_t body_bytes = len - i;
        rx_write(c, data + i, body_bytes);
        c->body_received += body_bytes;

        if (c->rx_count > 0)
            c->pending |= HTTP_CB_REQUEST;

        // Check for Content-Length completion
        if (c->content_length >= 0 &&
            (int32_t)c->body_received >= c->content_length) {
            c->state    = HTTP_STATE_DONE;
            c->pending |= HTTP_CB_COMPLETE;
        }
    }
}

// ── lwIP TCP callbacks ────────────────────────────────────────────────────────

#ifdef WIFI_ENABLED

static bool setup_tcp(http_conn_t *c, const ip_addr_t *addr) {
    c->state = HTTP_STATE_CONNECTING;

    c->pcb = tcp_new();
    if (!c->pcb) { conn_fail(c, "tcp_new() failed"); return false; }

    tcp_arg(c->pcb, c);
    tcp_recv(c->pcb, tcp_recv_cb);
    tcp_sent(c->pcb, tcp_sent_cb);
    tcp_err(c->pcb, tcp_err_cb);
    tcp_poll(c->pcb, tcp_poll_cb, 4);   // fires every 4 × 500ms = 2 s

    c->deadline_connect = now_ms() + c->connect_timeout_ms;

    err_t e = tcp_connect(c->pcb, addr, c->port, tcp_connected_cb);
    if (e != ERR_OK) {
        tcp_abort(c->pcb); c->pcb = NULL;
        conn_fail(c, "tcp_connect failed: %d", (int)e);
        return false;
    }
    return true;
}

static void dns_found_cb(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    http_conn_t *c = (http_conn_t *)arg;
    if (c->state != HTTP_STATE_DNS) return;

    if (!addr) {
        conn_fail(c, "DNS: no address for %s", c->server);
        return;
    }
    setup_tcp(c, addr);
}

static err_t try_send(http_conn_t *c, struct tcp_pcb *pcb) {
    while (c->tx_sent < c->tx_len) {
        u16_t space = tcp_sndbuf(pcb);
        if (space == 0) break;

        uint32_t remaining = c->tx_len - c->tx_sent;
        u16_t    chunk     = (remaining < (uint32_t)space) ? (u16_t)remaining : space;

        err_t e = tcp_write(pcb, c->tx_buf + c->tx_sent, chunk, TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK) {
            c->pcb = NULL;
            conn_fail(c, "tcp_write failed: %d", (int)e);
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        c->tx_sent += chunk;
    }
    tcp_output(pcb);

    if (c->tx_sent >= c->tx_len) {
        // All request data written to TCP send buffer — safe to free
        free(c->tx_buf); c->tx_buf = NULL;
        c->state = HTTP_STATE_HEADERS;
    }
    return ERR_OK;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    http_conn_t *c = (http_conn_t *)arg;

    if (err != ERR_OK) {
        c->pcb = NULL;
        conn_fail(c, "TCP connect error: %d", (int)err);
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    c->state         = HTTP_STATE_SENDING;
    c->deadline_read = now_ms() + c->read_timeout_ms;
    return try_send(c, pcb);
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)len;
    http_conn_t *c = (http_conn_t *)arg;
    if (c->tx_sent < c->tx_len)
        return try_send(c, pcb);
    return ERR_OK;
}

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_conn_t *c = (http_conn_t *)arg;

    if (!p) {
        // Server closed the connection (FIN)
        if (c->state == HTTP_STATE_BODY && c->content_length < 0) {
            // No Content-Length: treat EOF as body complete
            c->state    = HTTP_STATE_DONE;
            c->pending |= HTTP_CB_COMPLETE;
        }
        c->pending |= HTTP_CB_CLOSED;
        c->pcb = NULL;
        tcp_close(pcb);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return ERR_OK;
    }

    // Reset read deadline on incoming data
    c->deadline_read = now_ms() + c->read_timeout_ms;

    // Process pbuf chain
    for (struct pbuf *q = p; q != NULL; q = q->next)
        process_recv(c, (const uint8_t *)q->payload, (uint32_t)q->len);

    // Abort if process_recv transitioned us to FAILED
    if (c->state == HTTP_STATE_FAILED) {
        pbuf_free(p);
        if (c->pcb) { c->pcb = NULL; tcp_abort(pcb); return ERR_ABRT; }
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err) {
    // IMPORTANT: pcb is already freed by lwIP when this fires.
    http_conn_t *c = (http_conn_t *)arg;
    c->pcb = NULL;  // pcb is gone — do not touch it
    if (c->state != HTTP_STATE_DONE && c->state != HTTP_STATE_FAILED) {
        conn_fail(c, "TCP error: %d", (int)err);
    }
}

static err_t tcp_poll_cb(void *arg, struct tcp_pcb *pcb) {
    http_conn_t *c = (http_conn_t *)arg;
    uint32_t t = now_ms();

    if (c->state == HTTP_STATE_CONNECTING && t > c->deadline_connect) {
        c->pcb = NULL;
        conn_fail(c, "Connect timeout");
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if ((c->state == HTTP_STATE_HEADERS || c->state == HTTP_STATE_BODY) &&
        t > c->deadline_read) {
        c->pcb = NULL;
        conn_fail(c, "Read timeout");
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

#endif // WIFI_ENABLED

// ── Public API ────────────────────────────────────────────────────────────────

void http_init(void) {
    memset(s_conns, 0, sizeof(s_conns));
}

void http_close_all(void) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (s_conns[i].in_use) {
            s_conns[i].lua_ud  = NULL;
            s_conns[i].pending = 0;
            http_free(&s_conns[i]);
        }
    }
}

http_conn_t *http_alloc(void) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (!s_conns[i].in_use) {
            memset(&s_conns[i], 0, sizeof(s_conns[i]));
            s_conns[i].in_use            = true;
            s_conns[i].range_from        = -1;
            s_conns[i].range_to          = -1;
            s_conns[i].connect_timeout_ms = 10000;
            s_conns[i].read_timeout_ms    = 30000;
            s_conns[i].content_length     = -1;

            s_conns[i].hdr_buf = malloc(HTTP_HEADER_BUF_MAX);
            s_conns[i].rx_buf  = malloc(HTTP_RECV_BUF_DEFAULT);
            s_conns[i].rx_cap  = HTTP_RECV_BUF_DEFAULT;

            if (!s_conns[i].hdr_buf || !s_conns[i].rx_buf) {
                free(s_conns[i].hdr_buf); s_conns[i].hdr_buf = NULL;
                free(s_conns[i].rx_buf);  s_conns[i].rx_buf  = NULL;
                s_conns[i].in_use = false;
                return NULL;
            }
            return &s_conns[i];
        }
    }
    return NULL;
}

void http_close(http_conn_t *c) {
    if (!c) return;
#ifdef WIFI_ENABLED
    if (c->pcb) {
        cyw43_arch_lwip_begin();
        // Detach all callbacks before closing to prevent stale firings
        tcp_arg(c->pcb,  NULL);
        tcp_recv(c->pcb, NULL);
        tcp_sent(c->pcb, NULL);
        tcp_err(c->pcb,  NULL);
        tcp_poll(c->pcb, NULL, 0);
        err_t e = tcp_close(c->pcb);
        if (e != ERR_OK) tcp_abort(c->pcb);
        c->pcb = NULL;
        cyw43_arch_lwip_end();
    }
#endif
    free(c->tx_buf); c->tx_buf = NULL;
    c->tx_len    = 0;
    c->tx_sent   = 0;
    c->state     = HTTP_STATE_IDLE;
    c->pending   = 0;
}

void http_free(http_conn_t *c) {
    if (!c) return;
    http_close(c);
    free(c->rx_buf);  c->rx_buf  = NULL;
    free(c->hdr_buf); c->hdr_buf = NULL;
    memset(c, 0, sizeof(*c));
    // in_use cleared by memset
}

bool http_set_recv_buf(http_conn_t *c, uint32_t bytes) {
    if (!c || bytes == 0 || bytes > HTTP_RECV_BUF_MAX) return false;
    uint8_t *nb = realloc(c->rx_buf, bytes);
    if (!nb) return false;
    c->rx_buf   = nb;
    c->rx_cap   = bytes;
    c->rx_head  = 0;
    c->rx_tail  = 0;
    c->rx_count = 0;
    return true;
}

// ── Request issuing ───────────────────────────────────────────────────────────

static bool start_request(http_conn_t *c, const char *method, const char *path,
                           const char *extra_hdr, const char *body, size_t body_len) {
    if (!c) return false;

#ifndef WIFI_ENABLED
    conn_fail(c, "WiFi not compiled in");
    return false;
#else
    // Reset all per-request state
    c->state         = HTTP_STATE_DNS;
    c->err[0]        = '\0';
    c->pending       = 0;
    c->headers_done  = false;
    c->hdr_len       = 0;
    if (c->hdr_buf) c->hdr_buf[0] = '\0';
    c->status_code    = 0;
    c->content_length = -1;
    c->body_received  = 0;
    c->hdr_count      = 0;
    c->rx_head        = 0;
    c->rx_tail        = 0;
    c->rx_count       = 0;

    // Build HTTP request into tx_buf
    free(c->tx_buf);
    c->tx_buf = build_request(c, method, path, extra_hdr, body, body_len, &c->tx_len);
    if (!c->tx_buf) {
        conn_fail(c, "Request build failed (OOM)");
        return false;
    }
    c->tx_sent = 0;

    // Start DNS resolution
    cyw43_arch_lwip_begin();
    ip_addr_t addr;
    err_t e = dns_gethostbyname(c->server, &addr, dns_found_cb, c);
    if (e == ERR_OK) {
        // Cached result — dns_found_cb was NOT called; proceed directly
        setup_tcp(c, &addr);
    } else if (e != ERR_INPROGRESS) {
        free(c->tx_buf); c->tx_buf = NULL;
        cyw43_arch_lwip_end();
        conn_fail(c, "DNS failed: %d", (int)e);
        return false;
    }
    cyw43_arch_lwip_end();

    return (c->state != HTTP_STATE_FAILED);
#endif
}

bool http_get(http_conn_t *c, const char *path, const char *extra_hdr) {
    return start_request(c, "GET", path, extra_hdr, NULL, 0);
}

bool http_post(http_conn_t *c, const char *path, const char *extra_hdr,
               const char *body, size_t body_len) {
    return start_request(c, "POST", path, extra_hdr, body, body_len);
}

// ── Read / status accessors ───────────────────────────────────────────────────

uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len) {
    if (!c || !out || len == 0 || c->rx_count == 0) return 0;
    uint32_t n = (len < c->rx_count) ? len : c->rx_count;
    for (uint32_t i = 0; i < n; i++) {
        out[i]    = c->rx_buf[c->rx_tail];
        c->rx_tail = (c->rx_tail + 1) % c->rx_cap;
    }
    c->rx_count -= n;
    return n;
}

uint32_t http_bytes_available(http_conn_t *c) {
    return c ? c->rx_count : 0;
}

http_conn_t *http_get_conn(int idx) {
    if (idx < 0 || idx >= HTTP_MAX_CONNECTIONS) return NULL;
    return s_conns[idx].in_use ? &s_conns[idx] : NULL;
}

uint8_t http_take_pending(http_conn_t *c) {
    if (!c) return 0;
    uint8_t p = c->pending;
    c->pending = 0;
    return p;
}
