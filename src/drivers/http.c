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

// RP2350 XIP cache is per-core with no hardware coherency for PSRAM.
// Core 1 writes response data to rx_buf; Core 0 reads it.  Both must
// access rx_buf through the uncached alias (0x15xxxxxx) so writes go
// straight to physical PSRAM and reads bypass stale cache lines.
#define PSRAM_UNCACHED_OFFSET 0x04000000u
static inline uint8_t *rx_buf_uncached(const uint8_t *cached_ptr) {
  return (uint8_t *)((uintptr_t)cached_ptr + PSRAM_UNCACHED_OFFSET);
}

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

// Atomically set pending callback bits (called from Core 1)
static inline void pending_set(http_conn_t *c, uint8_t bits) {
  uint32_t irq = spin_lock_blocking(c->rx_spinlock);
  c->pending |= bits;
  spin_unlock(c->rx_spinlock, irq);
}

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
  pending_set(c, HTTP_CB_FAILED | HTTP_CB_CLOSED);
}

// Returns actual bytes written (may be less than len if buffer is full)
static uint32_t rx_write(http_conn_t *c, const uint8_t *data, uint32_t len) {
  uint32_t irq = spin_lock_blocking(c->rx_spinlock);

  uint32_t space = c->rx_cap - c->rx_count;
  if (len > space)
    len = space;
  if (len == 0) {
    spin_unlock(c->rx_spinlock, irq);
    return 0;
  }

  // Write through uncached alias — bypasses Core 1's XIP cache so data
  // reaches physical PSRAM immediately, visible to Core 0's uncached reads.
  uint8_t *uc = rx_buf_uncached(c->rx_buf);
  uint32_t till_end = c->rx_cap - c->rx_head;
  if (len <= till_end) {
    memcpy(&uc[c->rx_head], data, len);
    c->rx_head += len;
    if (c->rx_head == c->rx_cap)
      c->rx_head = 0;
  } else {
    memcpy(&uc[c->rx_head], data, till_end);
    memcpy(uc, data + till_end, len - till_end);
    c->rx_head = len - till_end;
  }
  c->rx_count += len;

  spin_unlock(c->rx_spinlock, irq);
  return len;
}

// ── Build and send HTTP request in a single PSRAM buffer ─────────────────────
// Uses umm_malloc (PSRAM) to assemble the full request, then mg_send() to
// append it to the Mongoose send iobuf in one resize.

void http_build_and_send_request(struct mg_connection *nc, http_conn_t *c) {
  size_t path_len = c->path ? strlen(c->path) : 1;
  size_t hdrs_len = c->extra_hdrs ? strlen(c->extra_hdrs) : 0;
  size_t need = path_len + strlen(c->server) + hdrs_len + 256;
  if (c->tx_buf && c->tx_len > 0)
    need += 32 + c->tx_len;

  char *buf = umm_malloc(need);
  if (!buf) {
    conn_fail(c, "request build OOM");
    return;
  }

  // Skip default User-Agent / Accept-Encoding if extra_hdrs already provides one
  bool has_ua = c->extra_hdrs &&
      (strstr(c->extra_hdrs, "User-Agent:") != NULL ||
       strstr(c->extra_hdrs, "user-agent:") != NULL);
  bool has_ae = c->extra_hdrs &&
      (strstr(c->extra_hdrs, "Accept-Encoding:") != NULL ||
       strstr(c->extra_hdrs, "accept-encoding:") != NULL);

  int off = snprintf(buf, need,
      "%s %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "%s"
      "%s"
      "Connection: %s\r\n",
      c->method, c->path, c->server,
      has_ua ? "" : "User-Agent: PicOS/1.0\r\n",
      has_ae ? "" : "Accept-Encoding: identity\r\n",
      c->keep_alive ? "keep-alive" : "close");

  if (c->extra_hdrs) {
    off += snprintf(buf + off, need - off, "%s", c->extra_hdrs);
    umm_free(c->extra_hdrs);
    c->extra_hdrs = NULL;
  }

  if (c->tx_buf && c->tx_len > 0) {
    off += snprintf(buf + off, need - off,
        "Content-Length: %u\r\n\r\n", (unsigned)c->tx_len);
    memcpy(buf + off, c->tx_buf, c->tx_len);
    off += c->tx_len;
    umm_free(c->tx_buf);
    c->tx_buf = NULL;
  } else {
    off += snprintf(buf + off, need - off, "\r\n");
  }

  mg_send(nc, buf, off);
  umm_free(buf);
}

// ── Mongoose Event Handler ───────────────────────────────────────────────────
// Non-static: called by wifi.c drain_requests() via mg_http_connect().
// Runs exclusively on Core 1 inside mg_mgr_poll().
// Supports streaming: fires HTTP_CB_REQUEST incrementally as data arrives.

void http_ev_fn(struct mg_connection *nc, int ev, void *ev_data) {
  http_conn_t *c = (http_conn_t *)nc->fn_data;
  if (!c)
    return;

  if (ev == MG_EV_CONNECT) {
    c->state = HTTP_STATE_SENDING;
    c->deadline_connect = 0; // connected — cancel connect timeout
    c->deadline_read = to_ms_since_boot(get_absolute_time()) + c->read_timeout_ms;
    printf("[HTTP] Connected, sending %s (%u bytes) %s\n", c->method,
           (unsigned)strlen(c->path), c->path);
    http_build_and_send_request(nc, c);
  } else if (ev == MG_EV_HTTP_HDRS) {
    if (c->headers_done)
      return;  // Already processed headers for this connection
    // Headers parsed - extract status code and Content-Length
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    c->status_code = atoi(hm->message.buf + 9);
    printf("[HTTP] Headers received, status %d\n", c->status_code);

    struct mg_str *cl = mg_http_get_header(hm, "Content-Length");
    if (cl) {
      c->content_length = atoi(cl->buf);
      printf("[HTTP] Content-Length %d\n", c->content_length);
    }

    // Parse response headers into hdr_keys/hdr_vals for Lua access.
    // Must be done here (not MG_EV_HTTP_MSG) because streaming detach
    // below prevents MG_EV_HTTP_MSG from firing for large responses.
    c->hdr_count = 0;
    size_t hdr_off = 0;
    for (int i = 0; i < MG_MAX_HTTP_HEADERS && hm->headers[i].name.len > 0;
         i++) {
      struct mg_http_header *h = &hm->headers[i];
      size_t need = h->name.len + 1 + h->value.len + 1;
      if (hdr_off + need > HTTP_HEADER_BUF_MAX) {
        printf("[HTTP] Header buf overflow: need %u, have %u/%u\n",
               (unsigned)need, (unsigned)hdr_off,
               (unsigned)HTTP_HEADER_BUF_MAX);
        break;
      }
      if (c->hdr_count >= HTTP_MAX_HDR_ENTRIES)
        break;

      // Copy name (lowercased for consistent Lua lookups)
      c->hdr_keys[c->hdr_count] = &c->hdr_buf[hdr_off];
      for (size_t j = 0; j < h->name.len; j++)
        c->hdr_buf[hdr_off++] =
            (h->name.buf[j] >= 'A' && h->name.buf[j] <= 'Z')
                ? h->name.buf[j] + 32
                : h->name.buf[j];
      c->hdr_buf[hdr_off++] = '\0';

      // Copy value
      c->hdr_vals[c->hdr_count] = &c->hdr_buf[hdr_off];
      memcpy(&c->hdr_buf[hdr_off], h->value.buf, h->value.len);
      hdr_off += h->value.len;
      c->hdr_buf[hdr_off++] = '\0';

      c->hdr_count++;
    }
    c->hdr_len = hdr_off;

    // Copy any body bytes that arrived with the headers.
    // IMPORTANT: hm->body.len is the Content-Length (total expected), NOT the
    // bytes currently in the recv buffer.  Actual body data available is:
    //   recv.len - (body_start - recv_start)
    // Copying hm->body.len would read past received data into zero-filled
    // mg_calloc memory, producing a body of mostly null bytes.
    {
      size_t body_offset = (size_t)(hm->body.buf - (char *)nc->recv.buf);
      size_t actual_body = (nc->recv.len > body_offset)
                               ? nc->recv.len - body_offset
                               : 0;
      if (actual_body > hm->body.len)
        actual_body = hm->body.len;  // never exceed Content-Length
      if (actual_body > 0) {
        uint32_t written =
            rx_write(c, (const uint8_t *)hm->body.buf, (uint32_t)actual_body);
        if (written > 0) {
          c->body_received += written;
          pending_set(c, HTTP_CB_REQUEST);
        }
      }
    }

    c->headers_done = true;
    c->state = HTTP_STATE_BODY;
    c->deadline_read = to_ms_since_boot(get_absolute_time()) + c->read_timeout_ms;
    pending_set(c, HTTP_CB_HEADERS);

    // Trigger Mongoose streaming detach: clear the recv buffer so Mongoose
    // sees recv.len changed (mongoose.c:2663) and sets pfn=NULL. All
    // subsequent data arrives as raw MG_EV_READ events instead of being
    // buffered for MG_EV_HTTP_MSG — critical for large downloads.
    mg_iobuf_del(&nc->recv, 0, nc->recv.len);

    // Check if the entire body already arrived with headers (small response)
    if (c->content_length >= 0 &&
        c->body_received >= (uint32_t)c->content_length) {
      c->state = HTTP_STATE_DONE;
      pending_set(c, HTTP_CB_COMPLETE);
      if (!c->keep_alive) nc->is_closing = 1;
    }
  } else if (ev == MG_EV_READ && c->headers_done && c->state != HTTP_STATE_DONE) {
    // STREAMING: Copy body data incrementally to our rx_buf.
    // After MG_EV_HTTP_HDRS detach, all body data arrives here as raw reads.
    uint32_t avail = (uint32_t)nc->recv.len;
    if (avail > 0 && c->rx_count < c->rx_cap) {
      uint32_t space = c->rx_cap - c->rx_count;
      uint32_t to_copy = (avail < space) ? avail : space;

      if (to_copy > 0) {
        uint32_t written = rx_write(c, nc->recv.buf, to_copy);
        if (written > 0) {
          mg_iobuf_del(&nc->recv, 0, written);  // FREE only what was actually copied
          c->body_received += written;
          c->deadline_read = to_ms_since_boot(get_absolute_time()) + c->read_timeout_ms;
          pending_set(c, HTTP_CB_REQUEST);  // Fire Lua callback with new data
        }
      }

      // Check for download completion (streaming mode)
      if (c->content_length >= 0 &&
          c->body_received >= (uint32_t)c->content_length) {
        c->state = HTTP_STATE_DONE;
        pending_set(c, HTTP_CB_COMPLETE);
        if (!c->keep_alive) nc->is_closing = 1;
      }
    }
    // If rx_buf is full, don't consume from nc->recv — TCP backpressure
    // will pause the sender until Lua drains the buffer via http_read()
  } else if (ev == MG_EV_HTTP_MSG) {
    // This only fires for non-streamed connections (where detach didn't happen,
    // e.g. chunked encoding without Content-Length). Streamed connections
    // complete via MG_EV_READ completion check above.
    if (c->headers_done)
      return;  // Already handled via streaming detach in MG_EV_HTTP_HDRS

    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    printf("[HTTP] Response complete (non-streamed), status %d, body_len %zu\n",
           atoi(hm->message.buf + 9), hm->body.len);

    c->status_code = atoi(hm->message.buf + 9);

    // Log error response bodies for diagnostics
    if (c->status_code >= 400 && hm->body.len > 0) {
      size_t show = hm->body.len < 256 ? hm->body.len : 256;
      printf("[HTTP] Error body: %.*s\n", (int)show, hm->body.buf);
    }

    // Parse response headers into hdr_keys/hdr_vals for Lua access
    c->hdr_count = 0;
    size_t hdr_off = 0;
    for (int i = 0; i < MG_MAX_HTTP_HEADERS && hm->headers[i].name.len > 0;
         i++) {
      struct mg_http_header *h = &hm->headers[i];
      size_t need = h->name.len + 1 + h->value.len + 1;
      if (hdr_off + need > HTTP_HEADER_BUF_MAX) {
        printf("[HTTP] Header buf overflow: need %u, have %u/%u\n",
               (unsigned)need, (unsigned)hdr_off,
               (unsigned)HTTP_HEADER_BUF_MAX);
        break;
      }
      if (c->hdr_count >= HTTP_MAX_HDR_ENTRIES)
        break;

      // Copy name (lowercased for consistent Lua lookups)
      c->hdr_keys[c->hdr_count] = &c->hdr_buf[hdr_off];
      for (size_t j = 0; j < h->name.len; j++)
        c->hdr_buf[hdr_off++] =
            (h->name.buf[j] >= 'A' && h->name.buf[j] <= 'Z')
                ? h->name.buf[j] + 32
                : h->name.buf[j];
      c->hdr_buf[hdr_off++] = '\0';

      // Copy value
      c->hdr_vals[c->hdr_count] = &c->hdr_buf[hdr_off];
      memcpy(&c->hdr_buf[hdr_off], h->value.buf, h->value.len);
      hdr_off += h->value.len;
      c->hdr_buf[hdr_off++] = '\0';

      c->hdr_count++;
    }
    c->hdr_len = hdr_off;

    // Copy body data
    if (hm->body.len > 0) {
      uint32_t written = rx_write(c, (uint8_t *)hm->body.buf, (uint32_t)hm->body.len);
      c->body_received += written;
    }

    c->headers_done = true;
    c->state = HTTP_STATE_DONE;
    pending_set(c, HTTP_CB_HEADERS | HTTP_CB_COMPLETE);

    if (!c->keep_alive) {
      nc->is_closing = 1;
    }
  } else if (ev == MG_EV_ERROR) {
    // If we've received any body data (streaming), treat as partial success
    // TLS errors after some data received are often benign (late errors)
    uint32_t pending_data = (nc && c->headers_done) ? (uint32_t)nc->recv.len : 0;
    if (c->state == HTTP_STATE_DONE) {
      // Already processed, ignore
    } else if (c->body_received > 0 || pending_data > 0) {
      // Got some data - copy any pending data and treat as partial success
      printf("[HTTP] Partial response: got %u bytes, %u pending in buffer\n", 
             c->body_received, pending_data);
      if (pending_data > 0) {
        uint32_t written = rx_write(c, nc->recv.buf, pending_data);
        if (written > 0) {
          mg_iobuf_del(&nc->recv, 0, written);
          c->body_received += written;
        }
      }
      // Fire both REQUEST (for data) and COMPLETE (for done)
      c->state = HTTP_STATE_DONE;
      pending_set(c, HTTP_CB_REQUEST | HTTP_CB_COMPLETE);
      nc->is_closing = 1;
    } else {
      // No data received, treat as failure
      conn_fail(c, "Mongoose error: %s", (char *)ev_data);
    }
  } else if (ev == MG_EV_CLOSE) {
    printf("[HTTP] Connection closed (slot %ld, state %d)\n",
           (long)(c - s_conns), (int)c->state);
    if (c->state != HTTP_STATE_DONE && c->state != HTTP_STATE_FAILED) {
      pending_set(c, HTTP_CB_CLOSED);
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
      // Claim a hardware spinlock for cross-core ring buffer protection
      s_conns[i].rx_spin_num = spin_lock_claim_unused(true);
      s_conns[i].rx_spinlock = spin_lock_instance(s_conns[i].rx_spin_num);
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
  uint32_t irq = spin_lock_blocking(c->rx_spinlock);
  c->pending = 0;
  spin_unlock(c->rx_spinlock, irq);
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

  umm_free(c->path);
  c->path = NULL;
  umm_free(c->extra_hdrs);
  c->extra_hdrs = NULL;
  umm_free(c->tx_buf);
  c->tx_buf = NULL;
  umm_free(c->rx_buf);
  umm_free(c->hdr_buf);
  // Unclaim spinlock before zeroing the struct
  if (c->rx_spinlock) {
    spin_lock_unclaim(c->rx_spin_num);
  }
  memset(c, 0, sizeof(*c));
}

bool http_set_recv_buf(http_conn_t *c, uint32_t bytes) {
  if (!c || bytes == 0 || bytes > HTTP_RECV_BUF_MAX)
    return false;
  // Use free + malloc instead of realloc to avoid memcpy creating stale
  // XIP cache entries on Core 0.  Core 1 writes response data to rx_buf;
  // if Core 0's cache has entries from a realloc copy, it reads stale data
  // (RP2350 has per-core XIP caches, no hardware coherency for PSRAM).
  uint32_t irq = spin_lock_blocking(c->rx_spinlock);
  umm_free(c->rx_buf);
  c->rx_buf = umm_malloc(bytes);
  if (!c->rx_buf) {
    c->rx_cap = 0;
    spin_unlock(c->rx_spinlock, irq);
    return false;
  }
  c->rx_cap = bytes;
  c->rx_head = 0;
  c->rx_tail = 0;
  c->rx_count = 0;
  spin_unlock(c->rx_spinlock, irq);
  return true;
}

static bool start_request(http_conn_t *c, const char *method, const char *path,
                          const char *extra_hdr, const char *body,
                          size_t body_len) {
  if (!c)
    return false;

  if (!wifi_is_available())
    return false;

  // Reset state for a new request — hold spinlock while resetting ring buffer
  c->status_code = 0;
  c->content_length = -1;
  c->body_received = 0;
  c->headers_done = false;
  c->err[0] = '\0';
  {
    uint32_t irq = spin_lock_blocking(c->rx_spinlock);
    c->rx_head = 0;
    c->rx_tail = 0;
    c->rx_count = 0;
    c->pending = 0;
    spin_unlock(c->rx_spinlock, irq);
  }

  strncpy(c->method, method, sizeof(c->method) - 1);

  umm_free(c->path);
  c->path = http_strdup(path);
  if (!c->path) {
    conn_fail(c, "path alloc failed");
    return false;
  }

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
  c->deadline_connect = to_ms_since_boot(get_absolute_time()) + c->connect_timeout_ms;
  c->deadline_read = 0;

  // Push to Core 1's request queue — it will call mg_http_connect() /
  // mg_printf() / etc. from within drain_requests().
  conn_req_t req = {.type = CONN_REQ_HTTP_START, .conn = c};
  if (!wifi_req_push(&req)) {
    // Queue full — fail immediately and release buffers
    umm_free(c->path);
    c->path = NULL;
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
  if (!c || !out || len == 0)
    return 0;

  uint32_t irq = spin_lock_blocking(c->rx_spinlock);

  if (c->rx_count == 0) {
    spin_unlock(c->rx_spinlock, irq);
    return 0;
  }
  uint32_t n = (len < c->rx_count) ? len : c->rx_count;

  // Read through uncached alias — bypasses Core 0's XIP cache to see
  // fresh data written by Core 1 to physical PSRAM.
  const uint8_t *uc = rx_buf_uncached(c->rx_buf);
  uint32_t till_end = c->rx_cap - c->rx_tail;
  if (n <= till_end) {
    memcpy(out, &uc[c->rx_tail], n);
    c->rx_tail += n;
    if (c->rx_tail == c->rx_cap)
      c->rx_tail = 0;
  } else {
    memcpy(out, &uc[c->rx_tail], till_end);
    memcpy(out + till_end, uc, n - till_end);
    c->rx_tail = n - till_end;
  }
  c->rx_count -= n;

  spin_unlock(c->rx_spinlock, irq);

  return n;
}

uint32_t http_bytes_available(http_conn_t *c) {
  if (!c) return 0;
  uint32_t irq = spin_lock_blocking(c->rx_spinlock);
  uint32_t n = c->rx_count;
  spin_unlock(c->rx_spinlock, irq);
  return n;
}

http_conn_t *http_get_conn(int idx) {
  return (idx >= 0 && idx < HTTP_MAX_CONNECTIONS && s_conns[idx].in_use)
             ? &s_conns[idx]
             : NULL;
}

uint8_t http_take_pending(http_conn_t *c) {
  if (!c)
    return 0;
  uint32_t irq = spin_lock_blocking(c->rx_spinlock);
  uint8_t p = c->pending;
  c->pending = 0;
  spin_unlock(c->rx_spinlock, irq);
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

// ── Timeout enforcement (called from wifi_poll on Core 1) ───────────────────

void http_check_timeouts(void) {
  uint32_t now = to_ms_since_boot(get_absolute_time());
  for (int i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
    http_conn_t *c = &s_conns[i];
    if (!c->in_use) continue;

    // Connect timeout: covers QUEUED → CONNECTING → SENDING
    if ((c->state == HTTP_STATE_QUEUED || c->state == HTTP_STATE_CONNECTING ||
         c->state == HTTP_STATE_SENDING) &&
        c->deadline_connect > 0 && now > c->deadline_connect) {
      conn_fail(c, "connect timeout (%ums)", (unsigned)c->connect_timeout_ms);
      if (c->pcb) {
        ((struct mg_connection *)c->pcb)->is_closing = 1;
        c->pcb = NULL;
      }
      c->deadline_connect = 0;
    }
    // Read timeout: covers HEADERS and BODY (idle — no data arriving)
    else if ((c->state == HTTP_STATE_HEADERS || c->state == HTTP_STATE_BODY) &&
             c->deadline_read > 0 && now > c->deadline_read) {
      conn_fail(c, "read timeout (%ums)", (unsigned)c->read_timeout_ms);
      if (c->pcb) {
        ((struct mg_connection *)c->pcb)->is_closing = 1;
        c->pcb = NULL;
      }
      c->deadline_read = 0;
    }
  }
}

// ── Custom Mongoose Allocator ────────────────────────────────────────────────
void *mg_calloc(size_t count, size_t size) { return umm_calloc(count, size); }

void mg_free(void *ptr) { umm_free(ptr); }
