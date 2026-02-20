#include "http.h"
#include "display.h"
#include "pico/stdlib.h"
#include "mongoose.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Forward decl from wifi.c
struct mg_mgr *wifi_get_mgr(void);

// ── Static pool ───────────────────────────────────────────────────────────────

static http_conn_t s_conns[HTTP_MAX_CONNECTIONS];

// ── Internal helpers ──────────────────────────────────────────────────────────

static void conn_fail(http_conn_t *c, const char *fmt, ...) {
    // If we already finished the request successfully, ignore late errors
    if (c->state == HTTP_STATE_DONE) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof(c->err), fmt, ap);
    va_end(ap);
    printf("[HTTP] Error (state %d): %s\n", (int)c->state, c->err);
    c->state   = HTTP_STATE_FAILED;
    c->pending |= HTTP_CB_FAILED | HTTP_CB_CLOSED;
}

static void rx_write(http_conn_t *c, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (c->rx_count >= c->rx_cap) break;
        c->rx_buf[c->rx_head] = data[i];
        c->rx_head = (c->rx_head + 1) % c->rx_cap;
        c->rx_count++;
    }
}

// ── Mongoose Event Handler ───────────────────────────────────────────────────

static void fn(struct mg_connection *nc, int ev, void *ev_data) {
    http_conn_t *c = (http_conn_t *)nc->fn_data;
    if (!c) return;

    if (ev == MG_EV_CONNECT) {
        c->state = HTTP_STATE_SENDING;
        printf("[HTTP] Connected, sending %s %s\n", c->method, c->path);
        
        mg_printf(nc, "%s %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: PicOS/1.0\r\n"
                      "Connection: close\r\n",
                      c->method, c->path, c->server);
        
        if (c->extra_hdrs) {
            mg_printf(nc, "%s", c->extra_hdrs);
        }
        
        if (c->tx_buf && c->tx_len > 0) {
            mg_printf(nc, "Content-Length: %u\r\n\r\n", (unsigned)c->tx_len);
            mg_send(nc, c->tx_buf, c->tx_len);
        } else {
            mg_printf(nc, "\r\n");
        }
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        printf("[HTTP] Response received, status %d, body_len %zu\n", 
               atoi(hm->message.buf + 9), hm->body.len);
        
        c->status_code = atoi(hm->message.buf + 9);
        
        // Extract Content-Length for the Lua progress indicators
        struct mg_str *cl = mg_http_get_header(hm, "Content-Length");
        if (cl) {
            c->content_length = atoi(cl->buf);
        }

        c->headers_done = true;
        c->state = HTTP_STATE_BODY;
        c->pending |= HTTP_CB_HEADERS | HTTP_CB_REQUEST;

        // Copy body into our ring buffer for Lua's conn:read()
        rx_write(c, (uint8_t *)hm->body.buf, (uint32_t)hm->body.len);
        c->body_received = (uint32_t)hm->body.len;
        
        c->state = HTTP_STATE_DONE;
        c->pending |= HTTP_CB_COMPLETE;
        
        nc->is_closing = 1; 
    } else if (ev == MG_EV_ERROR) {
        // If state is DONE, we got the data, so ignore this error (often late TLS recv error)
        if (c->state != HTTP_STATE_DONE) {
            conn_fail(c, "Mongoose error: %s", (char *)ev_data);
        }
    } else if (ev == MG_EV_CLOSE) {
        printf("[HTTP] Connection closed (slot %ld, state %d)\n", (long)(c - s_conns), (int)c->state);
        if (c->state != HTTP_STATE_DONE && c->state != HTTP_STATE_FAILED) {
            c->pending |= HTTP_CB_CLOSED;
        }
        c->pcb = NULL;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void http_init(void) {
    memset(s_conns, 0, sizeof(s_conns));
}

void http_close_all(void (*on_free)(void *lua_ud)) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (s_conns[i].in_use) {
            if (on_free && s_conns[i].lua_ud) on_free(s_conns[i].lua_ud);
            http_free(&s_conns[i]);
        }
    }
}

http_conn_t *http_alloc(void) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (!s_conns[i].in_use) {
            memset(&s_conns[i], 0, sizeof(s_conns[i]));
            s_conns[i].in_use = true;
            s_conns[i].range_from = -1;
            s_conns[i].range_to = -1;
            s_conns[i].connect_timeout_ms = 10000;
            s_conns[i].read_timeout_ms = 30000;
            s_conns[i].hdr_buf = malloc(HTTP_HEADER_BUF_MAX);
            s_conns[i].rx_buf = malloc(HTTP_RECV_BUF_DEFAULT);
            s_conns[i].rx_cap = HTTP_RECV_BUF_DEFAULT;
            if (!s_conns[i].hdr_buf || !s_conns[i].rx_buf) {
                printf("[HTTP] Failed to allocate buffers for connection %d (OOM)\n", i);
                http_free(&s_conns[i]);
                return NULL;
            }
            printf("[HTTP] Allocated connection %d\n", i);
            return &s_conns[i];
        }
    }
    printf("[HTTP] Failed to allocate connection: all %d slots in use\n", HTTP_MAX_CONNECTIONS);
    return NULL;
}

void http_close(http_conn_t *c) {
    if (!c) return;
    if (c->pcb) {
        struct mg_connection *nc = (struct mg_connection *)c->pcb;
        nc->is_closing = 1;
        c->pcb = NULL;
    }
    free(c->extra_hdrs); c->extra_hdrs = NULL;
    free(c->tx_buf); c->tx_buf = NULL;
    c->state = HTTP_STATE_IDLE;
    c->pending = 0;
}

void http_free(http_conn_t *c) {
    if (!c) return;
    http_close(c);
    free(c->rx_buf);
    free(c->hdr_buf);
    memset(c, 0, sizeof(*c));
}

bool http_set_recv_buf(http_conn_t *c, uint32_t bytes) {
    if (!c || bytes == 0 || bytes > HTTP_RECV_BUF_MAX) return false;
    uint8_t *nb = realloc(c->rx_buf, bytes);
    if (!nb) return false;
    c->rx_buf = nb;
    c->rx_cap = bytes;
    c->rx_head = 0;
    c->rx_tail = 0;
    c->rx_count = 0;
    return true;
}

static bool start_request(http_conn_t *c, const char *method, const char *path,
                           const char *extra_hdr, const char *body, size_t body_len) {
    if (!c) return false;
    
    struct mg_mgr *mgr = wifi_get_mgr();
    if (!mgr) return false;

    // Reset state
    c->status_code = 0;
    c->content_length = -1;
    c->body_received = 0;
    c->headers_done = false;
    c->rx_head = 0;
    c->rx_tail = 0;
    c->rx_count = 0;
    c->err[0] = '\0';

    strncpy(c->method, method, sizeof(c->method)-1);
    strncpy(c->path, path, sizeof(c->path)-1);
    c->extra_hdrs = extra_hdr ? strdup(extra_hdr) : NULL;
    c->tx_buf = body ? strdup(body) : NULL;
    c->tx_len = (uint32_t)body_len;

    char url[320];
    snprintf(url, sizeof(url), "%s://%s:%u", c->use_ssl ? "https" : "http", c->server, c->port);

    printf("[HTTP] Connecting to %s (SSL=%d)...\n", url, c->use_ssl);
    struct mg_connection *nc = mg_http_connect(mgr, url, fn, c);
    if (!nc) {
        conn_fail(c, "mg_http_connect failed");
        return false;
    }

    if (c->use_ssl) {
        struct mg_tls_opts opts = {0};
        opts.name = mg_str(c->server);
        mg_tls_init(nc, &opts);
    }

    c->pcb = (void *)nc;
    c->state = HTTP_STATE_CONNECTING;
    
    return true;
}

bool http_get(http_conn_t *c, const char *path, const char *extra_hdr) {
    return start_request(c, "GET", path, extra_hdr, NULL, 0);
}

bool http_post(http_conn_t *c, const char *path, const char *extra_hdr, const char *body, size_t body_len) {
    return start_request(c, "POST", path, extra_hdr, body, body_len);
}

uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len) {
    if (!c || !out || len == 0 || c->rx_count == 0) return 0;
    uint32_t n = (len < c->rx_count) ? len : c->rx_count;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = c->rx_buf[c->rx_tail];
        c->rx_tail = (c->rx_tail + 1) % c->rx_cap;
    }
    c->rx_count -= n;
    return n;
}

uint32_t http_bytes_available(http_conn_t *c) {
    return c ? c->rx_count : 0;
}

http_conn_t *http_get_conn(int idx) {
    return (idx >= 0 && idx < HTTP_MAX_CONNECTIONS && s_conns[idx].in_use) ? &s_conns[idx] : NULL;
}

uint8_t http_take_pending(http_conn_t *c) {
    if (!c) return 0;
    uint8_t p = c->pending;
    c->pending = 0;
    return p;
}

void http_poll(void) {
}
