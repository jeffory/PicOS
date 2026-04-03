#include "wifi.h"
#include "tcp.h"
#include "../os/clock.h"
#include "../os/config.h"
#include "../os/system_menu.h"
#include "../os/toast.h"
#include "display.h"
#include "http.h"

#include "mongoose.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ── State
// ─────────────────────────────────────────────────────────────────────

static bool s_available = false;
static _Atomic wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static char s_ssid[64] = {0};
static char s_pass[64] = {0};
static char s_ip[20] = {0};
static _Atomic bool s_hw_disconnected = true; // true once mg_wifi_disconnect() has completed
static bool s_http_required = false;
static volatile bool s_disconnect_pending = false; // deferred disconnect from SNTP callback
static bool s_auto_connected = false;    // true only for boot auto-connect

// Connect timeout / retry
static uint32_t s_connect_start_ms = 0;
static int      s_connect_retries  = 0;
#define WIFI_CONNECT_TIMEOUT_MS  15000  // 15 seconds per attempt
#define WIFI_MAX_RETRIES         3

// SNTP retry
static uint8_t  s_sntp_retries = 0;
static uint32_t s_sntp_next_retry_ms = 0;
#define SNTP_MAX_RETRIES    3
#define SNTP_RETRY_DELAY_MS 5000

static struct mg_mgr s_mgr;
static struct mg_tcpip_if s_ifp;
static struct mg_tcpip_driver_pico_w_data s_driver_data;

// ── Core 0 → Core 1 request queue
// ────────────────────────────────────────

#define REQ_QUEUE_SIZE 8
#define REQ_QUEUE_MASK (REQ_QUEUE_SIZE - 1)

static conn_req_t      s_req_queue[REQ_QUEUE_SIZE];
static _Atomic uint8_t s_req_head = 0;  // written by Core 0 (producer)
static _Atomic uint8_t s_req_tail = 0;  // written by Core 1 (consumer)

// Separate spinlock for WiFi state (s_ip, s_ssid) — NOT for the queue
static spin_lock_t *s_state_lock;

bool wifi_req_push(const conn_req_t *req) {
  uint8_t head = atomic_load_explicit(&s_req_head, memory_order_relaxed);
  uint8_t next = (head + 1) & REQ_QUEUE_MASK;
  uint8_t tail = atomic_load_explicit(&s_req_tail, memory_order_acquire);

  if (next == tail) {
    printf("WiFi: request queue full, dropping request type=%d\n", (int)req->type);
    return false;
  }

  s_req_queue[head] = *req;
  atomic_store_explicit(&s_req_head, next, memory_order_release);

  // Ring doorbell to wake Core 1 immediately (<1us) instead of waiting
  // for the 5ms polling timer.  Core 1's doorbell ISR sets the tick flag.
  multicore_doorbell_set_other_core(WIFI_IPC_DOORBELL);

  return true;
}

// ── Internet connectivity check
// ──────────────────────────────────────────────────────────────────────
// After WiFi associates and gets an IP, we verify actual internet
// reachability with a lightweight TCP connect to 8.8.8.8:53 (Google DNS).
// No DNS dependency, no TLS, minimal overhead.

static _Atomic bool     s_internet_ok = false;
static uint32_t s_connectivity_check_ms = 0;
static struct mg_connection *s_check_conn = NULL;

#define CONNECTIVITY_CHECK_INTERVAL_MS  60000   // recheck every 60s
#define CONNECTIVITY_CHECK_RETRY_MS     10000   // retry faster on failure

static void connectivity_cb(struct mg_connection *c, int ev, void *ev_data) {
  (void)ev_data;
  if (ev == MG_EV_CONNECT) {
    printf("WiFi: connectivity check OK\n");
    s_internet_ok = true;
    s_check_conn = NULL;
    c->is_closing = 1;
    s_connectivity_check_ms =
        to_ms_since_boot(get_absolute_time()) + CONNECTIVITY_CHECK_INTERVAL_MS;
  } else if (ev == MG_EV_ERROR) {
    printf("WiFi: connectivity check failed: %s\n", (char *)ev_data);
    s_internet_ok = false;
    s_check_conn = NULL;
    c->is_closing = 1;
    toast_push("No internet connection", TOAST_STYLE_WARNING);
    s_connectivity_check_ms =
        to_ms_since_boot(get_absolute_time()) + CONNECTIVITY_CHECK_RETRY_MS;
  } else if (ev == MG_EV_CLOSE) {
    s_check_conn = NULL;
  }
}

static void start_connectivity_check(void) {
  if (s_check_conn) return; // already in progress
  s_check_conn = mg_connect(&s_mgr, "tcp://8.8.8.8:53", connectivity_cb, NULL);
  if (!s_check_conn) {
    s_internet_ok = false;
    s_connectivity_check_ms =
        to_ms_since_boot(get_absolute_time()) + CONNECTIVITY_CHECK_RETRY_MS;
  }
}

// ── SNTP
// ──────────────────────────────────────────────────────────────────────

static void sntp_cb(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_SNTP_TIME) {
    int64_t *t = (int64_t *)ev_data;
    printf("WiFi: SNTP sync OK, time: %lld\n", *t);
    clock_sntp_set((unsigned)(*t / 1000));
    s_sntp_retries = 0;
    c->is_closing = 1;
    if (!s_http_required && s_auto_connected &&
        system_menu_get_wifi_auto_disconnect()) {
      // Don't call mg_wifi_disconnect() directly here — we're inside
      // mg_mgr_poll() and calling it from within a Mongoose callback causes
      // reentrancy into the CYW43 driver. Set a flag; wifi_poll() will
      // perform the disconnect safely after mg_mgr_poll() returns.
      s_disconnect_pending = true;
    }
  } else if (ev == MG_EV_ERROR) {
    printf("WiFi: SNTP error: %s\n", (char *)ev_data);
    c->is_closing = 1;
    if (s_sntp_retries < SNTP_MAX_RETRIES) {
      s_sntp_retries++;
      s_sntp_next_retry_ms = to_ms_since_boot(get_absolute_time()) + SNTP_RETRY_DELAY_MS;
      printf("WiFi: SNTP retry %d/%d in %dms\n", s_sntp_retries, SNTP_MAX_RETRIES,
             SNTP_RETRY_DELAY_MS);
    } else {
      printf("WiFi: SNTP failed after %d retries\n", SNTP_MAX_RETRIES);
      toast_push("Time sync failed", TOAST_STYLE_ERROR);
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
      s_connect_start_ms = 0;
      s_connect_retries = 0;
      s_internet_ok = false;
      // Schedule connectivity check 2s after IP assignment
      s_connectivity_check_ms = to_ms_since_boot(get_absolute_time()) + 2000;
      { uint32_t save = spin_lock_blocking(s_state_lock);
        mg_snprintf(s_ip, sizeof(s_ip), "%M", mg_print_ip, &ifp->ip);
        spin_unlock(s_state_lock, save);
      }
      printf("WiFi: connected  IP=%s\n", s_ip);
      start_sntp();
    } else if (state == MG_TCPIP_STATE_DOWN) {
      if (s_status == WIFI_STATUS_CONNECTED) {
        s_status = WIFI_STATUS_DISCONNECTED;
        { uint32_t save = spin_lock_blocking(s_state_lock);
          s_ip[0] = '\0';
          spin_unlock(s_state_lock, save);
        }
        s_internet_ok = false;
        s_connectivity_check_ms = 0;
        printf("WiFi: disconnected\n");
        toast_push("WiFi disconnected", TOAST_STYLE_WARNING);
      }
    }
  } else if (ev == MG_TCPIP_EV_WIFI_CONNECT_ERR) {
    s_status = WIFI_STATUS_FAILED;
    s_connect_start_ms = 0;
    printf("WiFi: connect failed (err=%d)\n", *(int *)ev_data);
  }
}

// ── Core 1 request drainer
// ────────────────────────────────────────────────

// Called by wifi_poll() on Core 1 before mg_mgr_poll().
// Executes queued mg_* operations that Core 0 cannot safely call directly.
static void drain_requests(void) {
  for (;;) {
    uint8_t tail = atomic_load_explicit(&s_req_tail, memory_order_relaxed);
    uint8_t head = atomic_load_explicit(&s_req_head, memory_order_acquire);

    if (head == tail)
      break;

    conn_req_t req = s_req_queue[tail];
    atomic_store_explicit(&s_req_tail, (tail + 1) & REQ_QUEUE_MASK,
                          memory_order_release);

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
          http_build_and_send_request(nc, c);
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
        // Clear fn_data BEFORE marking for close.  If the pool slot is
        // freed (http_free) and reallocated before Mongoose fires
        // MG_EV_CLOSE, fn_data would point to the NEW connection's
        // struct — causing use-after-free corruption.  The null check
        // in http_ev_fn() safely skips the stale MG_EV_CLOSE.
        nc->fn_data = NULL;
        nc->is_closing = 1;
        c->pcb = NULL;
        break;
      }

      case CONN_REQ_TCP_CONNECT: {
        tcp_conn_t *tc = (tcp_conn_t *)req.conn;
        if (tc) {
          char url[320];
          snprintf(url, sizeof(url), "%s://%s:%u",
                   tc->use_ssl ? "tls" : "tcp", tc->host, tc->port);
          printf("[TCP] Connecting to %s (SSL=%d)...\n", url, tc->use_ssl);
          tc->state = TCP_STATE_CONNECTING;
          struct mg_connection *nc = mg_connect(&s_mgr, url, tcp_ev_fn, tc);
          if (!nc) {
            printf("[TCP] mg_connect failed\n");
            tc->state = TCP_STATE_FAILED;
            snprintf(tc->err, sizeof(tc->err), "mg_connect failed");
            break;
          }
          tc->pcb = (void *)nc;
        }
        break;
      }

      case CONN_REQ_TCP_WRITE: {
        tcp_conn_t *tc = (tcp_conn_t *)req.conn;
        if (tc && tc->pcb && req.data) {
          mg_send((struct mg_connection *)tc->pcb, req.data, req.data_len);
        }
        if (req.data) umm_free(req.data);
        break;
      }

      case CONN_REQ_TCP_CLOSE: {
        tcp_conn_t *tc = (tcp_conn_t *)req.conn;
        if (tc && tc->pcb) {
          ((struct mg_connection *)tc->pcb)->is_closing = 1;
          tc->pcb = NULL;
        }
        break;
      }

      case CONN_REQ_WIFI_CONNECT:
        s_driver_data.wifi.ssid = s_ssid;
        s_driver_data.wifi.pass = s_pass;
        mg_wifi_connect(&s_driver_data.wifi);
        break;

      case CONN_REQ_WIFI_DISCONNECT:
        mg_wifi_disconnect();
        atomic_store_explicit(&s_hw_disconnected, true, memory_order_release);
        break;
    }
  }
}

// ── Public API
// ─────────────────────────────────────────────────────────────────

void wifi_init(void) {
  // Claim a hardware spinlock for WiFi state protection (s_ip, s_ssid)
  int lock_num = spin_lock_claim_unused(true);
  s_state_lock = spin_lock_instance(lock_num);
  atomic_store(&s_req_head, 0);
  atomic_store(&s_req_tail, 0);

  // Claim doorbell for IPC wake-up (Core 0 rings, Core 1 handles)
  multicore_doorbell_claim(WIFI_IPC_DOORBELL, 0x3);  // both cores

  mg_mgr_init(&s_mgr);

  memset(&s_ifp, 0, sizeof(s_ifp));
  memset(&s_driver_data, 0, sizeof(s_driver_data));

  s_ifp.driver = &mg_tcpip_driver_pico_w;
  s_ifp.driver_data = &s_driver_data;
  s_ifp.pfn = tcpip_cb;
  s_ifp.recv_queue.size = 8192;

  mg_tcpip_init(&s_mgr, &s_ifp);
  s_ifp.pfn = tcpip_cb; // Ensure our callback is set

  if (s_mgr.ifp == NULL) {
    printf("WiFi: driver init failed — CYW43 not available\n");
    s_available = false;
    return;
  }

  s_available = true;
  printf("WiFi: Mongoose TCPIP ready\n");

  // Auto-connect if credentials are stored in /system/config.json.
  const char *ssid = config_get("wifi_ssid");
  const char *pass = config_get("wifi_pass");
  if (ssid && ssid[0]) {
    printf("WiFi: auto-connecting to '%s'\n", ssid);
    wifi_connect(ssid, pass ? pass : "");
    s_auto_connected = true; // mark as boot auto-connect (after wifi_connect clears it)
  }
}

bool wifi_is_available(void) { return s_available; }

void wifi_connect(const char *ssid, const char *password) {
  if (!s_available || !ssid || !ssid[0])
    return;

  { uint32_t save = spin_lock_blocking(s_state_lock);
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';

    if (password) {
      strncpy(s_pass, password, sizeof(s_pass) - 1);
      s_pass[sizeof(s_pass) - 1] = '\0';
    } else {
      s_pass[0] = '\0';
    }

    s_ip[0] = '\0';
    spin_unlock(s_state_lock, save);
  }
  s_status = WIFI_STATUS_CONNECTING;
  s_connect_start_ms = to_ms_since_boot(get_absolute_time());
  s_connect_retries = 0;
  s_auto_connected = false; // user/app-initiated, don't auto-disconnect after SNTP

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
  atomic_store_explicit(&s_hw_disconnected, false, memory_order_release);
  s_ssid[0] = '\0';
  s_ip[0] = '\0';
  printf("WiFi: disconnect queued\n");

  // Queue the mg_wifi_disconnect() for Core 1 to execute
  conn_req_t req = {.type = CONN_REQ_WIFI_DISCONNECT};
  wifi_req_push(&req);
}

wifi_status_t wifi_get_status(void) {
  if (s_ifp.state == MG_TCPIP_STATE_READY)
    return s_internet_ok ? WIFI_STATUS_ONLINE : WIFI_STATUS_CONNECTED;
  return s_status;
}

bool wifi_has_internet(void) {
  return s_internet_ok && s_ifp.state == MG_TCPIP_STATE_READY;
}

bool wifi_hw_disconnected(void) {
  return atomic_load_explicit(&s_hw_disconnected, memory_order_acquire);
}

const char *wifi_get_ip(void) {
  wifi_status_t st = wifi_get_status();
  if (st != WIFI_STATUS_CONNECTED && st != WIFI_STATUS_ONLINE)
    return NULL;
  // Copy under lock to prevent torn reads from Core 1 writes
  static char s_ip_copy[20];
  uint32_t save = spin_lock_blocking(s_state_lock);
  mg_snprintf(s_ip, sizeof(s_ip), "%M", mg_print_ip, &s_ifp.ip);
  memcpy(s_ip_copy, s_ip, sizeof(s_ip_copy));
  spin_unlock(s_state_lock, save);
  return s_ip_copy[0] ? s_ip_copy : NULL;
}

const char *wifi_get_ssid(void) {
  static char s_ssid_copy[64];
  uint32_t save = spin_lock_blocking(s_state_lock);
  memcpy(s_ssid_copy, s_ssid, sizeof(s_ssid_copy));
  spin_unlock(s_state_lock, save);
  return s_ssid_copy[0] ? s_ssid_copy : NULL;
}

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

  // Always drain requests — connect requests must be processed even when
  // disconnected/failed, otherwise queued WIFI_CONNECT never executes.
  drain_requests();

  // Skip Mongoose polling when disconnected to save power
  wifi_status_t st = wifi_get_status();
  if (st != WIFI_STATUS_CONNECTED && st != WIFI_STATUS_CONNECTING &&
      st != WIFI_STATUS_ONLINE) {
    return;
  }

  mg_mgr_poll(&s_mgr, 0);

  // Enforce HTTP and TCP connection/read timeouts
  http_check_timeouts();
  tcp_check_timeouts();

  // Connect timeout: if stuck in CONNECTING, retry or give up
  if (s_status == WIFI_STATUS_CONNECTING && s_connect_start_ms > 0) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - s_connect_start_ms > WIFI_CONNECT_TIMEOUT_MS) {
      if (s_connect_retries < WIFI_MAX_RETRIES) {
        s_connect_retries++;
        printf("WiFi: connect timeout, retry %d/%d\n",
               s_connect_retries, WIFI_MAX_RETRIES);
        mg_wifi_disconnect();
        s_driver_data.wifi.ssid = s_ssid;
        s_driver_data.wifi.pass = s_pass;
        mg_wifi_connect(&s_driver_data.wifi);
        s_connect_start_ms = now_ms;
      } else {
        printf("WiFi: connect failed after %d retries\n", WIFI_MAX_RETRIES);
        mg_wifi_disconnect();
        s_status = WIFI_STATUS_FAILED;
        toast_push("WiFi connection failed", TOAST_STYLE_WARNING);
        s_connect_start_ms = 0;
        s_connect_retries = 0;
      }
    }
  }

  // Internet connectivity check timer
  if (s_connectivity_check_ms > 0 &&
      (st == WIFI_STATUS_CONNECTED || st == WIFI_STATUS_ONLINE)) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms >= s_connectivity_check_ms) {
      s_connectivity_check_ms = 0;
      start_connectivity_check();
    }
  }

  // SNTP retry timer
  if (s_sntp_next_retry_ms > 0 &&
      (st == WIFI_STATUS_CONNECTED || st == WIFI_STATUS_ONLINE)) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms >= s_sntp_next_retry_ms) {
      s_sntp_next_retry_ms = 0;
      start_sntp();
    }
  }

  // Process any disconnect deferred from inside a Mongoose callback (SNTP etc.)
  // We're already on Core 1 here, so call mg_wifi_disconnect() directly.
  if (s_disconnect_pending) {
    s_disconnect_pending = false;
    mg_wifi_disconnect();
    s_status = WIFI_STATUS_DISCONNECTED;
    { uint32_t save = spin_lock_blocking(s_state_lock);
      s_ssid[0] = '\0';
      s_ip[0] = '\0';
      spin_unlock(s_state_lock, save);
    }
    s_internet_ok = false;
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
