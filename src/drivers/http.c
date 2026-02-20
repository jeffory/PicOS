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
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "mbedtls/ssl.h"
#endif

// ── Static pool ───────────────────────────────────────────────────────────────

static http_conn_t s_conns[HTTP_MAX_CONNECTIONS];

// ── TLS Configuration ────────────────────────────────────────────────────────
#ifdef WIFI_ENABLED
static struct altcp_tls_config *s_tls_config = NULL;
#endif

// ── Forward declarations ──────────────────────────────────────────────────────

#ifdef WIFI_ENABLED
static void  dns_found_cb(const char *name, const ip_addr_t *addr, void *arg);
static err_t http_connected_cb(void *arg, struct altcp_pcb *pcb, err_t err);
static err_t http_recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err);
static err_t http_sent_cb(void *arg, struct altcp_pcb *pcb, u16_t len);
static void  http_err_cb(void *arg, err_t err);
static err_t http_poll_cb(void *arg, struct altcp_pcb *pcb);
static err_t try_send(http_conn_t *c, struct altcp_pcb *pcb);
static bool  setup_pcb(http_conn_t *c, const ip_addr_t *addr);
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

static bool str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static void rx_write(http_conn_t *c, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (c->rx_count >= c->rx_cap) break;
        c->rx_buf[c->rx_head] = data[i];
        c->rx_head = (c->rx_head + 1) % c->rx_cap;
        c->rx_count++;
    }
}

static int parse_status_line(const char *line) {
    const char *p = strchr(line, ' ');
    return p ? (int)strtol(p + 1, NULL, 10) : 0;
}

static void parse_headers(http_conn_t *c) {
    c->hdr_count   = 0;
    c->status_code = 0;
    c->content_length = -1;

    char *p = c->hdr_buf;
    char *eol = strstr(p, "\r\n");
    if (!eol) return;
    c->status_code = parse_status_line(p);
    
    char status_buf[64];
    size_t line_len = (size_t)(eol - p);
    if (line_len > 63) line_len = 63;
    memcpy(status_buf, p, line_len);
    status_buf[line_len] = '\0';
    printf("[HTTP] Status: %s\n", status_buf);

    p = eol + 2;

    while (*p && c->hdr_count < HTTP_MAX_HDR_ENTRIES) {
        if (p[0] == '\r' && p[1] == '\n') break;
        eol = strstr(p, "\r\n");
        if (!eol) break;
        char *colon = (char *)memchr(p, ':', (size_t)(eol - p));
        if (colon) {
            char *kend = colon;
            while (kend > p && kend[-1] == ' ') kend--;
            *kend = '\0';
            char *val = colon + 1;
            while (*val == ' ') val++;
            *eol = '\0';
            c->hdr_keys[c->hdr_count] = p;
            c->hdr_vals[c->hdr_count] = val;
            if (str_ieq(p, "content-length")) c->content_length = (int32_t)strtol(val, NULL, 10);
            c->hdr_count++;
        }
        p = eol + 2;
    }
}

static char *build_request(http_conn_t *c, const char *method, const char *path,
                            const char *extra_hdr, const char *body, size_t body_len,
                            uint32_t *out_len) {
    size_t max = 1024 + (path ? strlen(path) : 0) + body_len;
    char *buf = malloc(max);
    if (!buf) return NULL;
    int n = 0;
    n += snprintf(buf + n, max - n, "%s %s HTTP/1.1\r\n", method, path);
    n += snprintf(buf + n, max - n, "Host: %s\r\n", c->server);
    n += snprintf(buf + n, max - n, "User-Agent: PicOS/1.0\r\n");
    n += snprintf(buf + n, max - n, "Connection: close\r\n");
    if (body_len > 0) n += snprintf(buf + n, max - n, "Content-Length: %u\r\n", (unsigned)body_len);
    if (extra_hdr && *extra_hdr) n += snprintf(buf + n, max - n, "%s\r\n", extra_hdr);
    n += snprintf(buf + n, max - n, "\r\n");
    if (body && body_len > 0) { memcpy(buf + n, body, body_len); n += (int)body_len; }
    *out_len = (uint32_t)n;
    return buf;
}

static void process_recv(http_conn_t *c, const uint8_t *data, uint32_t len) {
    uint32_t i = 0;
    while (i < len && !c->headers_done) {
        if (c->hdr_len >= HTTP_HEADER_BUF_MAX - 1) { conn_fail(c, "Headers too large"); return; }
        c->hdr_buf[c->hdr_len++] = (char)data[i++];
        c->hdr_buf[c->hdr_len] = '\0';
        if (c->hdr_len >= 4) {
            const char *end = c->hdr_buf + c->hdr_len;
            if (end[-4]=='\r' && end[-3]=='\n' && end[-2]=='\r' && end[-1]=='\n') {
                c->headers_done = true;
                c->state = HTTP_STATE_BODY;
                parse_headers(c);
                c->pending |= HTTP_CB_HEADERS | HTTP_CB_REQUEST;
            }
        }
    }
    if (c->headers_done && i < len) {
        uint32_t body_bytes = len - i;
        rx_write(c, data + i, body_bytes);
        c->body_received += body_bytes;
        c->pending |= HTTP_CB_REQUEST;
        if (c->content_length >= 0 && (int32_t)c->body_received >= c->content_length) {
            c->state = HTTP_STATE_DONE;
            c->pending |= HTTP_CB_COMPLETE;
        }
    }
}

#ifdef WIFI_ENABLED
static bool setup_pcb(http_conn_t *c, const ip_addr_t *addr) {
    if (c->use_ssl) {
        if (!s_tls_config) {
            s_tls_config = altcp_tls_create_config_client(NULL, 0);
            if (!s_tls_config) { conn_fail(c, "TLS config failed"); return false; }
        }
        c->pcb = altcp_tls_new(s_tls_config, IPADDR_TYPE_ANY);
        if (c->pcb) {
            void *ssl_ctx = altcp_tls_context(c->pcb);
            if (ssl_ctx) {
                mbedtls_ssl_set_hostname((mbedtls_ssl_context *)ssl_ctx, c->server);
            }
        }
    } else {
        c->pcb = altcp_tcp_new_ip_type(IPADDR_TYPE_ANY);
    }
    if (!c->pcb) { conn_fail(c, "altcp_new failed"); return false; }
    altcp_arg(c->pcb, c);
    altcp_recv(c->pcb, http_recv_cb);
    altcp_sent(c->pcb, http_sent_cb);
    altcp_err(c->pcb, http_err_cb);
    altcp_poll(c->pcb, http_poll_cb, 4);
    c->deadline_connect = now_ms() + c->connect_timeout_ms;
    printf("[HTTP] Connecting to %s:%d (SSL=%d)...\n", ipaddr_ntoa(addr), c->port, c->use_ssl);
    if (altcp_connect(c->pcb, addr, c->port, http_connected_cb) != ERR_OK) {
        altcp_abort(c->pcb); c->pcb = NULL;
        conn_fail(c, "Connect failed");
        return false;
    }
    return true;
}

static void dns_found_cb(const char *name, const ip_addr_t *addr, void *arg) {
    http_conn_t *c = (http_conn_t *)arg;
    if (c->state != HTTP_STATE_DNS) return;
    if (!addr) { conn_fail(c, "DNS failed"); return; }
    setup_pcb(c, addr);
}

static err_t try_send(http_conn_t *c, struct altcp_pcb *pcb) {
    while (c->tx_sent < c->tx_len) {
        u16_t space = altcp_sndbuf(pcb);
        if (space == 0) break;
        uint32_t rem = c->tx_len - c->tx_sent;
        u16_t chunk = (rem < space) ? (u16_t)rem : space;
        if (altcp_write(pcb, c->tx_buf + c->tx_sent, chunk, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            altcp_abort(pcb); c->pcb = NULL;
            conn_fail(c, "Write failed");
            return ERR_ABRT;
        }
        c->tx_sent += chunk;
    }
    altcp_output(pcb);
    if (c->tx_sent >= c->tx_len) { free(c->tx_buf); c->tx_buf = NULL; c->state = HTTP_STATE_HEADERS; }
    return ERR_OK;
}

static err_t http_connected_cb(void *arg, struct altcp_pcb *pcb, err_t err) {
    http_conn_t *c = (http_conn_t *)arg;
    if (err != ERR_OK) { conn_fail(c, "Connect failed"); return ERR_ABRT; }
    printf("[HTTP] Connected\n");
    c->state = HTTP_STATE_SENDING;
    c->deadline_read = now_ms() + c->read_timeout_ms;
    return try_send(c, pcb);
}

static err_t http_sent_cb(void *arg, struct altcp_pcb *pcb, u16_t len) {
    http_conn_t *c = (http_conn_t *)arg;
    return (c->tx_sent < c->tx_len) ? try_send(c, pcb) : ERR_OK;
}

static err_t http_recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_conn_t *c = (http_conn_t *)arg;
    if (!p) {
        printf("[HTTP] Server closed\n");
        if (c->state == HTTP_STATE_BODY || c->state == HTTP_STATE_HEADERS) { c->state = HTTP_STATE_DONE; c->pending |= HTTP_CB_COMPLETE; }
        c->pending |= HTTP_CB_CLOSED; c->pcb = NULL;
        altcp_close(pcb); return ERR_OK;
    }
    c->deadline_read = now_ms() + c->read_timeout_ms;
    for (struct pbuf *q = p; q != NULL; q = q->next) process_recv(c, (const uint8_t *)q->payload, (uint32_t)q->len);
    altcp_recved(pcb, p->tot_len); pbuf_free(p);
    return ERR_OK;
}

static void http_err_cb(void *arg, err_t err) {
    http_conn_t *c = (http_conn_t *)arg;
    printf("[HTTP] async error: %d\n", (int)err);
    c->pcb = NULL;
    if (c->state != HTTP_STATE_DONE && c->state != HTTP_STATE_FAILED) conn_fail(c, "Network error");
}

static err_t http_poll_cb(void *arg, struct altcp_pcb *pcb) {
    http_conn_t *c = (http_conn_t *)arg;
    uint32_t t = now_ms();
    if (c->state == HTTP_STATE_CONNECTING && t > c->deadline_connect) { conn_fail(c, "Connect timeout"); altcp_abort(pcb); return ERR_ABRT; }
    if ((c->state == HTTP_STATE_HEADERS || c->state == HTTP_STATE_BODY) && t > c->deadline_read) { conn_fail(c, "Read timeout"); altcp_abort(pcb); return ERR_ABRT; }
    return ERR_OK;
}
#endif

void http_init(void) { memset(s_conns, 0, sizeof(s_conns)); }
void http_close_all(void) { for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) { if (s_conns[i].in_use) { s_conns[i].lua_ud = NULL; s_conns[i].pending = 0; http_free(&s_conns[i]); } } }

http_conn_t *http_alloc(void) {
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (!s_conns[i].in_use) {
            memset(&s_conns[i], 0, sizeof(s_conns[i]));
            s_conns[i].in_use = true; s_conns[i].range_from = -1; s_conns[i].range_to = -1;
            s_conns[i].connect_timeout_ms = 10000; s_conns[i].read_timeout_ms = 30000;
            s_conns[i].hdr_buf = malloc(HTTP_HEADER_BUF_MAX);
            s_conns[i].rx_buf = malloc(HTTP_RECV_BUF_DEFAULT); s_conns[i].rx_cap = HTTP_RECV_BUF_DEFAULT;
            if (!s_conns[i].hdr_buf || !s_conns[i].rx_buf) { http_free(&s_conns[i]); return NULL; }
            return &s_conns[i];
        }
    }
    return NULL;
}

void http_close(http_conn_t *c) {
    if (!c) return;
#ifdef WIFI_ENABLED
    if (c->pcb) {
        display_spi_lock();
        cyw43_arch_lwip_begin();
        altcp_arg(c->pcb, NULL); altcp_recv(c->pcb, NULL); altcp_sent(c->pcb, NULL); altcp_err(c->pcb, NULL); altcp_poll(c->pcb, NULL, 0);
        if (c->state == HTTP_STATE_CONNECTING || c->state == HTTP_STATE_DNS) altcp_abort(c->pcb);
        else if (altcp_close(c->pcb) != ERR_OK) altcp_abort(c->pcb);
        c->pcb = NULL; 
        cyw43_arch_lwip_end();
        display_spi_unlock();
    }
#endif
    free(c->tx_buf); c->tx_buf = NULL; c->state = HTTP_STATE_IDLE; c->pending = 0;
}

void http_free(http_conn_t *c) { if (!c) return; http_close(c); free(c->rx_buf); free(c->hdr_buf); memset(c, 0, sizeof(*c)); }

bool http_set_recv_buf(http_conn_t *c, uint32_t bytes) {
    if (!c || bytes == 0 || bytes > HTTP_RECV_BUF_MAX) return false;
    uint8_t *nb = realloc(c->rx_buf, bytes); if (!nb) return false;
    c->rx_buf = nb; c->rx_cap = bytes; c->rx_head = 0; c->rx_tail = 0; c->rx_count = 0; return true;
}

static bool start_request(http_conn_t *c, const char *method, const char *path,
                           const char *extra_hdr, const char *body, size_t body_len) {
    if (!c) return false;
#ifndef WIFI_ENABLED
    conn_fail(c, "No WiFi"); return false;
#else
    c->state = HTTP_STATE_IDLE; c->err[0] = '\0'; c->pending = 0; c->headers_done = false; c->hdr_len = 0;
    c->status_code = 0; c->content_length = -1; c->body_received = 0; c->hdr_count = 0;
    c->rx_head = 0; c->rx_tail = 0; c->rx_count = 0;
    c->tx_buf = build_request(c, method, path, extra_hdr, body, body_len, &c->tx_len);
    if (!c->tx_buf) { conn_fail(c, "OOM"); return false; }
    c->tx_sent = 0; c->state = HTTP_STATE_DNS;

    display_spi_lock();
    cyw43_arch_lwip_begin();
    ip_addr_t addr;
    err_t e = dns_gethostbyname(c->server, &addr, dns_found_cb, c);
    if (e == ERR_OK) setup_pcb(c, &addr);
    else if (e != ERR_INPROGRESS) { 
        free(c->tx_buf); c->tx_buf = NULL; 
        cyw43_arch_lwip_end(); 
        display_spi_unlock();
        conn_fail(c, "DNS failed"); return false; 
    }
    cyw43_arch_lwip_end();
    display_spi_unlock();
    return (c->state != HTTP_STATE_FAILED);
#endif
}

bool http_get(http_conn_t *c, const char *path, const char *extra_hdr) { return start_request(c, "GET", path, extra_hdr, NULL, 0); }
bool http_post(http_conn_t *c, const char *path, const char *extra_hdr, const char *body, size_t body_len) { return start_request(c, "POST", path, extra_hdr, body, body_len); }

uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len) {
    if (!c || !out || len == 0 || c->rx_count == 0) return 0;
    uint32_t n = (len < c->rx_count) ? len : c->rx_count;
    for (uint32_t i = 0; i < n; i++) { out[i] = c->rx_buf[c->rx_tail]; c->rx_tail = (c->rx_tail + 1) % c->rx_cap; }
    c->rx_count -= n; return n;
}

uint32_t http_bytes_available(http_conn_t *c) { return c ? c->rx_count : 0; }
http_conn_t *http_get_conn(int idx) { return (idx >= 0 && idx < HTTP_MAX_CONNECTIONS && s_conns[idx].in_use) ? &s_conns[idx] : NULL; }
uint8_t http_take_pending(http_conn_t *c) { if (!c) return 0; uint8_t p = c->pending; c->pending = 0; return p; }
