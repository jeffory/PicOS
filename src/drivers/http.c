#include "http.h"
#include "wifi.h"
#include "display.h"
#include "mbedtls/platform.h"
#include "mongoose.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "umm_malloc.h"

static char *http_strdup(const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s) + 1;
  char *d = umm_malloc(len);
  if (d)
    memcpy(d, s, len);
  return d;
}

// ── Static pool
// ───────────────────────────────────────────────────────────────

static http_conn_t s_conns[HTTP_MAX_CONNECTIONS];

// ── Internal helpers
// ──────────────────────────────────────────────────────────

static void conn_fail(http_conn_t *c, const char *fmt, ...) {
  // If we already finished the request successfully, ignore late errors
  if (c->state == HTTP_STATE_DONE)
    return;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->err, sizeof(c->err), fmt, ap);
  va_end(ap);
  printf("[HTTP] Error (state %d): %s\n", (int)c->state, c->err);
  c->state = HTTP_STATE_FAILED;
  c->pending |= HTTP_CB_FAILED | HTTP_CB_CLOSED;
}

static void rx_write(http_conn_t *c, const uint8_t *data, uint32_t len) {
  uint32_t space = c->rx_cap - c->rx_count;
  if (len > space)
    len = space;
  if (len == 0)
    return;

  uint32_t till_end = c->rx_cap - c->rx_head;
  if (len <= till_end) {
    memcpy(&c->rx_buf[c->rx_head], data, len);
    c->rx_head += len;
    if (c->rx_head == c->rx_cap)
      c->rx_head = 0;
  } else {
    memcpy(&c->rx_buf[c->rx_head], data, till_end);
    memcpy(c->rx_buf, data + till_end, len - till_end);
    c->rx_head = len - till_end;
  }
  c->rx_count += len;
}

// ── Mongoose Event Handler ───────────────────────────────────────────────────
// Non-static: called by wifi.c drain_requests() via mg_http_connect().
// Runs exclusively on Core 1 inside mg_mgr_poll().

void http_ev_fn(struct mg_connection *nc, int ev, void *ev_data) {
  http_conn_t *c = (http_conn_t *)nc->fn_data;
  if (!c)
    return;

  if (ev == MG_EV_CONNECT) {
    c->state = HTTP_STATE_SENDING;
    printf("[HTTP] Connected, sending %s %s\n", c->method, c->path);

    mg_printf(nc,
              "%s %s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "User-Agent: PicOS/1.0\r\n"
              "Connection: %s\r\n",
              c->method, c->path, c->server,
              c->keep_alive ? "keep-alive" : "close");

    if (c->extra_hdrs) {
      mg_printf(nc, "%s", c->extra_hdrs);
      umm_free(c->extra_hdrs);
      c->extra_hdrs = NULL;
    }

    if (c->tx_buf && c->tx_len > 0) {
      mg_printf(nc, "Content-Length: %u\r\n\r\n", (unsigned)c->tx_len);
      mg_send(nc, c->tx_buf, c->tx_len);
      umm_free(c->tx_buf);
      c->tx_buf = NULL;
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

    if (!c->keep_alive) {
      nc->is_closing = 1;
    }
  } else if (ev == MG_EV_ERROR) {
    // If state is DONE, we got the data, so ignore this error (often late TLS
    // recv error)
    if (c->state != HTTP_STATE_DONE) {
      conn_fail(c, "Mongoose error: %s", (char *)ev_data);
    }
  } else if (ev == MG_EV_CLOSE) {
    printf("[HTTP] Connection closed (slot %ld, state %d)\n",
           (long)(c - s_conns), (int)c->state);
    if (c->state != HTTP_STATE_DONE && c->state != HTTP_STATE_FAILED) {
      c->pending |= HTTP_CB_CLOSED;
    }
    c->pcb = NULL;
  }
}

// ── Public API
// ────────────────────────────────────────────────────────────────

void http_init(void) { memset(s_conns, 0, sizeof(s_conns)); }

void http_close_all(void (*on_free)(void *lua_ud)) {
  // Push CLOSE requests for all in-use connections.  If a CONN_REQ_HTTP_START
  // for any of these is already in the queue, the FIFO ordering guarantees
  // that Core 1 processes START before CLOSE — so c->pcb will be set before
  // the CLOSE is handled.
  for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
    if (s_conns[i].in_use) {
      conn_req_t req = {.type = CONN_REQ_HTTP_CLOSE, .conn = &s_conns[i]};
      wifi_req_push(&req);
    }
  }

  // Wait for all connections to close: pcb cleared (by CONN_REQ_HTTP_CLOSE
  // or MG_EV_CLOSE) and no connection still queued/pending.
  uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  bool any_pending;
  do {
    any_pending = false;
    for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
      if (s_conns[i].in_use &&
          (s_conns[i].pcb != NULL ||
           s_conns[i].state == HTTP_STATE_QUEUED)) {
        any_pending = true;
        break;
      }
    }
    if (any_pending)
      sleep_ms(5);
  } while (any_pending &&
           (to_ms_since_boot(get_absolute_time()) - start_ms) < 500);

  // Free all connections
  for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
    if (s_conns[i].in_use) {
      if (on_free && s_conns[i].lua_ud)
        on_free(s_conns[i].lua_ud);
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
      s_conns[i].hdr_buf = umm_malloc(HTTP_HEADER_BUF_MAX);
      s_conns[i].rx_buf = umm_malloc(HTTP_RECV_BUF_DEFAULT);
      s_conns[i].rx_cap = HTTP_RECV_BUF_DEFAULT;
      if (!s_conns[i].hdr_buf || !s_conns[i].rx_buf) {
        printf("[HTTP] Failed to allocate buffers for connection %d (OOM)\n",
               i);
        http_free(&s_conns[i]);
        return NULL;
      }
      printf("[HTTP] Allocated connection %d\n", i);
      return &s_conns[i];
    }
  }
  printf("[HTTP] Failed to allocate connection: all %d slots in use\n",
         HTTP_MAX_CONNECTIONS);
  return NULL;
}

void http_close(http_conn_t *c) {
  if (!c)
    return;
  if (c->pcb) {
    // Queue CLOSE for Core 1 — do not touch nc->is_closing from Core 0
    conn_req_t req = {.type = CONN_REQ_HTTP_CLOSE, .conn = c};
    wifi_req_push(&req);
    // Note: c->pcb is NOT cleared here; Core 1 clears it in
    // CONN_REQ_HTTP_CLOSE processing and again in MG_EV_CLOSE.
  }
  // Do not free extra_hdrs or tx_buf here: if a CONN_REQ_HTTP_START is in
  // the queue, Core 1 owns those buffers until after MG_EV_CONNECT fires.
  // They will be freed by http_ev_fn() MG_EV_CONNECT, drain_requests()
  // (keep-alive path), or http_free() after waiting for Core 1.
  if (c->state != HTTP_STATE_QUEUED) {
    c->state = HTTP_STATE_IDLE;
  }
  c->pending = 0;
}

void http_free(http_conn_t *c) {
  if (!c)
    return;

  // Enqueue a close if there is an active connection
  if (c->pcb) {
    conn_req_t req = {.type = CONN_REQ_HTTP_CLOSE, .conn = c};
    wifi_req_push(&req);
  }

  // Wait briefly for Core 1 to process any pending requests for this conn
  // so we don't free extra_hdrs/tx_buf while Core 1 is still using them.
  uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  while ((c->pcb != NULL || c->state == HTTP_STATE_QUEUED) &&
         (to_ms_since_boot(get_absolute_time()) - start_ms) < 200) {
    sleep_ms(1);
  }

  umm_free(c->extra_hdrs);
  c->extra_hdrs = NULL;
  umm_free(c->tx_buf);
  c->tx_buf = NULL;
  umm_free(c->rx_buf);
  umm_free(c->hdr_buf);
  memset(c, 0, sizeof(*c));
}

bool http_set_recv_buf(http_conn_t *c, uint32_t bytes) {
  if (!c || bytes == 0 || bytes > HTTP_RECV_BUF_MAX)
    return false;
  uint8_t *nb = umm_realloc(c->rx_buf, bytes);
  if (!nb)
    return false;
  c->rx_buf = nb;
  c->rx_cap = bytes;
  c->rx_head = 0;
  c->rx_tail = 0;
  c->rx_count = 0;
  return true;
}

static bool start_request(http_conn_t *c, const char *method, const char *path,
                          const char *extra_hdr, const char *body,
                          size_t body_len) {
  if (!c)
    return false;

  if (!wifi_is_available())
    return false;

  // Reset state for a new request
  c->status_code = 0;
  c->content_length = -1;
  c->body_received = 0;
  c->headers_done = false;
  c->rx_head = 0;
  c->rx_tail = 0;
  c->rx_count = 0;
  c->err[0] = '\0';
  c->pending = 0;

  strncpy(c->method, method, sizeof(c->method) - 1);
  strncpy(c->path, path, sizeof(c->path) - 1);

  // Allocate request buffers — ownership transfers to Core 1 at push time.
  // Core 1 frees them in drain_requests() (keep-alive) or http_ev_fn()
  // MG_EV_CONNECT (new connection).
  umm_free(c->extra_hdrs);
  c->extra_hdrs = extra_hdr ? http_strdup(extra_hdr) : NULL;
  umm_free(c->tx_buf);
  c->tx_buf = body ? http_strdup(body) : NULL;
  c->tx_len = (uint32_t)body_len;

  // Mark as queued so http_close_all() and http_free() know to wait
  c->state = HTTP_STATE_QUEUED;

  // Push to Core 1's request queue — it will call mg_http_connect() /
  // mg_printf() / etc. from within drain_requests().
  conn_req_t req = {.type = CONN_REQ_HTTP_START, .conn = c};
  if (!wifi_req_push(&req)) {
    // Queue full — fail immediately and release buffers
    umm_free(c->extra_hdrs);
    c->extra_hdrs = NULL;
    umm_free(c->tx_buf);
    c->tx_buf = NULL;
    conn_fail(c, "request queue full");
    return false;
  }

  return true;
}

bool http_get(http_conn_t *c, const char *path, const char *extra_hdr) {
  return start_request(c, "GET", path, extra_hdr, NULL, 0);
}

bool http_post(http_conn_t *c, const char *path, const char *extra_hdr,
               const char *body, size_t body_len) {
  return start_request(c, "POST", path, extra_hdr, body, body_len);
}

uint32_t http_read(http_conn_t *c, uint8_t *out, uint32_t len) {
  if (!c || !out || len == 0 || c->rx_count == 0)
    return 0;
  uint32_t n = (len < c->rx_count) ? len : c->rx_count;

  uint32_t till_end = c->rx_cap - c->rx_tail;
  if (n <= till_end) {
    memcpy(out, &c->rx_buf[c->rx_tail], n);
    c->rx_tail += n;
    if (c->rx_tail == c->rx_cap)
      c->rx_tail = 0;
  } else {
    memcpy(out, &c->rx_buf[c->rx_tail], till_end);
    memcpy(out + till_end, c->rx_buf, n - till_end);
    c->rx_tail = n - till_end;
  }
  c->rx_count -= n;
  return n;
}

uint32_t http_bytes_available(http_conn_t *c) { return c ? c->rx_count : 0; }

http_conn_t *http_get_conn(int idx) {
  return (idx >= 0 && idx < HTTP_MAX_CONNECTIONS && s_conns[idx].in_use)
             ? &s_conns[idx]
             : NULL;
}

uint8_t http_take_pending(http_conn_t *c) {
  if (!c)
    return 0;
  uint8_t p = c->pending;
  c->pending = 0;
  return p;
}

void http_poll(void) {}

// Fire C-language (non-Lua) HTTP callbacks.
// Native apps store their callback function pointers directly in http_conn_t
// extension fields (to be added when native HTTP is fully implemented).
// For now this is a no-op stub; the infrastructure is wired up so Core 1
// can call it safely every poll cycle.
void http_fire_c_pending(void) {
  // Future: iterate s_conns, check pending flags, call C callbacks.
}

// ── Custom Mongoose Allocator ────────────────────────────────────────────────
void *mg_calloc(size_t count, size_t size) { return umm_calloc(count, size); }

void mg_free(void *ptr) { umm_free(ptr); }
