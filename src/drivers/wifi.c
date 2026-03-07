#include "wifi.h"
#include "../os/clock.h"
#include "../os/config.h"
#include "display.h"
#include "http.h"

#include "mongoose.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

// ── State
// ─────────────────────────────────────────────────────────────────────

static bool s_available = false;
static wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static char s_ssid[64] = {0};
static char s_pass[64] = {0};
static char s_ip[20] = {0};
static bool s_http_required = false;
static bool s_disconnect_pending = false; // deferred disconnect from SNTP callback

static struct mg_mgr s_mgr;
static struct mg_tcpip_if s_ifp;
static struct mg_tcpip_driver_pico_w_data s_driver_data;

// ── Core 0 → Core 1 request queue
// ────────────────────────────────────────

#define REQ_QUEUE_SIZE 8
#define REQ_QUEUE_MASK (REQ_QUEUE_SIZE - 1)

static conn_req_t  s_req_queue[REQ_QUEUE_SIZE];
static uint8_t     s_req_head = 0;  // next write index (Core 0)
static uint8_t     s_req_tail = 0;  // next read  index (Core 1)
static spin_lock_t *s_req_lock;

bool wifi_req_push(const conn_req_t *req) {
  uint32_t save = spin_lock_blocking(s_req_lock);
  uint8_t next = (s_req_head + 1) & REQ_QUEUE_MASK;
  if (next == s_req_tail) {
    spin_unlock(s_req_lock, save);
    printf("WiFi: request queue full, dropping request type=%d\n", (int)req->type);
    return false;
  }
  s_req_queue[s_req_head] = *req;
  s_req_head = next;
  spin_unlock(s_req_lock, save);
  return true;
}

// ── SNTP
// ──────────────────────────────────────────────────────────────────────

static void sntp_cb(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_SNTP_TIME) {
    int64_t *t = (int64_t *)ev_data;
    printf("WiFi: SNTP sync OK, time: %lld\n", *t);
    clock_sntp_set((unsigned)(*t / 1000));
    c->is_closing = 1;
    if (!s_http_required) {
      // Don't call mg_wifi_disconnect() directly here — we're inside
      // mg_mgr_poll() and calling it from within a Mongoose callback causes
      // reentrancy into the CYW43 driver. Set a flag; wifi_poll() will
      // perform the disconnect safely after mg_mgr_poll() returns.
      s_disconnect_pending = true;
    }
  } else if (ev == MG_EV_CLOSE) {
    // SNTP closed
  }
}

static void start_sntp(void) {
  printf("WiFi: Starting SNTP sync...\n");
  mg_sntp_connect(&s_mgr, "udp://pool.ntp.org:123", sntp_cb, NULL);
}

// ── Internal helpers
// ──────────────────────────────────────────────────────────

static void tcpip_cb(struct mg_tcpip_if *ifp, int ev, void *ev_data) {
  if (ev == MG_TCPIP_EV_ST_CHG) {
    uint8_t state = *(uint8_t *)ev_data;
    if (state == MG_TCPIP_STATE_READY) {
      s_status = WIFI_STATUS_CONNECTED;
      mg_snprintf(s_ip, sizeof(s_ip), "%M", mg_print_ip, &ifp->ip);
      printf("WiFi: connected  IP=%s\n", s_ip);
      start_sntp();
    } else if (state == MG_TCPIP_STATE_DOWN) {
      if (s_status == WIFI_STATUS_CONNECTED) {
        s_status = WIFI_STATUS_DISCONNECTED;
        s_ip[0] = '\0';
        printf("WiFi: disconnected\n");
      }
    }
  } else if (ev == MG_TCPIP_EV_WIFI_CONNECT_ERR) {
    s_status = WIFI_STATUS_FAILED;
    printf("WiFi: connect failed (err=%d)\n", *(int *)ev_data);
  }
}

// ── Core 1 request drainer
// ────────────────────────────────────────────────

// Called by wifi_poll() on Core 1 before mg_mgr_poll().
// Executes queued mg_* operations that Core 0 cannot safely call directly.
static void drain_requests(void) {
  for (;;) {
    // Pop one entry under spinlock
    uint32_t save = spin_lock_blocking(s_req_lock);
    if (s_req_head == s_req_tail) {
      spin_unlock(s_req_lock, save);
      break;
    }
    conn_req_t req = s_req_queue[s_req_tail];
    s_req_tail = (s_req_tail + 1) & REQ_QUEUE_MASK;
    spin_unlock(s_req_lock, save);

    // Process without holding the spinlock
    switch (req.type) {

      case CONN_REQ_HTTP_START: {
        http_conn_t *c = req.conn;
        if (!c) break;

        if (c->keep_alive && c->pcb != NULL) {
          // Reuse existing keep-alive connection — send new request immediately
          struct mg_connection *nc = (struct mg_connection *)c->pcb;
          c->state = HTTP_STATE_SENDING;
          c->pending = 0;
          printf("[HTTP] Reusing connection for %s %s\n", c->method, c->path);
          mg_printf(nc,
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "User-Agent: PicOS/1.0\r\n"
                    "Connection: keep-alive\r\n",
                    c->method, c->path, c->server);
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
        } else {
          // New connection
          char url[320];
          snprintf(url, sizeof(url), "%s://%s:%u",
                   c->use_ssl ? "https" : "http", c->server, c->port);
          printf("[HTTP] Connecting to %s (SSL=%d)...\n", url, c->use_ssl);

          // Transition away from QUEUED *before* any mg_* call so that
          // http_close_all()'s busy-wait detects progress correctly.
          c->state = HTTP_STATE_CONNECTING;

          struct mg_connection *nc = mg_http_connect(&s_mgr, url, http_ev_fn, c);
          if (!nc) {
            printf("[HTTP] mg_http_connect failed\n");
            c->err[0] = '\0';
            snprintf(c->err, sizeof(c->err), "%s", "mg_http_connect failed");
            c->state = HTTP_STATE_FAILED;
            c->pending |= HTTP_CB_FAILED | HTTP_CB_CLOSED;
            // Free buffers now — fn() MG_EV_CONNECT will never fire
            umm_free(c->extra_hdrs); c->extra_hdrs = NULL;
            umm_free(c->tx_buf);     c->tx_buf = NULL;
            break;
          }

          if (c->use_ssl) {
            struct mg_tls_opts opts = {0};
            opts.name = mg_str(c->server);
            mg_tls_init(nc, &opts);
            if (!nc->is_tls_hs) {
              printf("[HTTP] TLS init failed\n");
              mg_close_conn(nc);
              snprintf(c->err, sizeof(c->err), "%s", "TLS init failed");
              c->state = HTTP_STATE_FAILED;
              c->pending |= HTTP_CB_FAILED | HTTP_CB_CLOSED;
              umm_free(c->extra_hdrs); c->extra_hdrs = NULL;
              umm_free(c->tx_buf);     c->tx_buf = NULL;
              break;
            }
          }

          c->pcb = (void *)nc;
          // extra_hdrs and tx_buf are freed in http_ev_fn() MG_EV_CONNECT
          // after the HTTP request is sent.
        }
        break;
      }

      case CONN_REQ_HTTP_CLOSE: {
        http_conn_t *c = req.conn;
        if (!c || !c->pcb) break;
        struct mg_connection *nc = (struct mg_connection *)c->pcb;
        nc->is_closing = 1;
        c->pcb = NULL;
        break;
      }

      case CONN_REQ_WIFI_CONNECT:
        s_driver_data.wifi.ssid = s_ssid;
        s_driver_data.wifi.pass = s_pass;
        mg_wifi_connect(&s_driver_data.wifi);
        break;

      case CONN_REQ_WIFI_DISCONNECT:
        mg_wifi_disconnect();
        break;
    }
  }
}

// ── Public API
// ─────────────────────────────────────────────────────────────────

void wifi_init(void) {
  // Claim a hardware spinlock for the request queue
  int lock_num = spin_lock_claim_unused(true);
  s_req_lock = spin_lock_instance(lock_num);
  s_req_head = 0;
  s_req_tail = 0;

  mg_mgr_init(&s_mgr);

  memset(&s_ifp, 0, sizeof(s_ifp));
  memset(&s_driver_data, 0, sizeof(s_driver_data));

  s_ifp.driver = &mg_tcpip_driver_pico_w;
  s_ifp.driver_data = &s_driver_data;
  s_ifp.pfn = tcpip_cb;
  s_ifp.recv_queue.size = 8192;

  mg_tcpip_init(&s_mgr, &s_ifp);
  s_ifp.pfn = tcpip_cb; // Ensure our callback is set

  s_available = true;
  printf("WiFi: Mongoose TCPIP ready\n");

  // Auto-connect if credentials are stored in /system/config.json.
  const char *ssid = config_get("wifi_ssid");
  const char *pass = config_get("wifi_pass");
  if (ssid && ssid[0]) {
    printf("WiFi: auto-connecting to '%s'\n", ssid);
    wifi_connect(ssid, pass ? pass : "");
  }
}

bool wifi_is_available(void) { return s_available; }

void wifi_connect(const char *ssid, const char *password) {
  if (!s_available || !ssid || !ssid[0])
    return;

  strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
  s_ssid[sizeof(s_ssid) - 1] = '\0';

  if (password) {
    strncpy(s_pass, password, sizeof(s_pass) - 1);
    s_pass[sizeof(s_pass) - 1] = '\0';
  } else {
    s_pass[0] = '\0';
  }

  s_status = WIFI_STATUS_CONNECTING;
  s_ip[0] = '\0';

  printf("WiFi: connecting to '%s'...\n", s_ssid);

  // Queue the mg_wifi_connect() for Core 1 to execute
  conn_req_t req = {.type = CONN_REQ_WIFI_CONNECT};
  wifi_req_push(&req);
}

void wifi_disconnect(void) {
  if (!s_available)
    return;

  // Update status immediately so callers see DISCONNECTED right away
  s_status = WIFI_STATUS_DISCONNECTED;
  s_ssid[0] = '\0';
  s_ip[0] = '\0';
  printf("WiFi: disconnect queued\n");

  // Queue the mg_wifi_disconnect() for Core 1 to execute
  conn_req_t req = {.type = CONN_REQ_WIFI_DISCONNECT};
  wifi_req_push(&req);
}

wifi_status_t wifi_get_status(void) {
  if (s_ifp.state == MG_TCPIP_STATE_READY)
    return WIFI_STATUS_CONNECTED;
  return s_status;
}

const char *wifi_get_ip(void) {
  if (wifi_get_status() != WIFI_STATUS_CONNECTED)
    return NULL;
  mg_snprintf(s_ip, sizeof(s_ip), "%M", mg_print_ip, &s_ifp.ip);
  return s_ip[0] ? s_ip : NULL;
}

const char *wifi_get_ssid(void) { return s_ssid[0] ? s_ssid : NULL; }

void wifi_set_http_required(bool required) { s_http_required = required; }
bool wifi_get_http_required(void) { return s_http_required; }

void wifi_poll(void) {
  if (!s_available)
    return;

  // Only Core 1 owns the Mongoose manager. All other callers are legacy
  // call sites from before Core 1 took ownership; they are now safe no-ops
  // since Core 1's core1_entry() drives the stack every 5 ms.
  if (get_core_num() != 1)
    return;

  // Skip polling when disconnected to save power
  wifi_status_t st = wifi_get_status();
  if (st != WIFI_STATUS_CONNECTED && st != WIFI_STATUS_CONNECTING)
    return;

  // Drain pending Core 0 requests before polling Mongoose
  drain_requests();

  mg_mgr_poll(&s_mgr, 0);

  // Process any disconnect deferred from inside a Mongoose callback (SNTP etc.)
  // We're already on Core 1 here, so call mg_wifi_disconnect() directly.
  if (s_disconnect_pending) {
    s_disconnect_pending = false;
    mg_wifi_disconnect();
    s_status = WIFI_STATUS_DISCONNECTED;
    s_ssid[0] = '\0';
    s_ip[0] = '\0';
    printf("WiFi: disconnected (SNTP deferred)\n");
  }
}

// mbedtls/time support
#include "mbedtls/platform_time.h"

mbedtls_ms_time_t mbedtls_platform_ms_time(void) {
  return (mbedtls_ms_time_t)(time_us_64() / 1000);
}

// Override time() for mbedtls certificate validation
time_t time(time_t *t) {
  time_t now = (time_t)clock_get_epoch();
  if (t)
    *t = now;
  return now;
}
