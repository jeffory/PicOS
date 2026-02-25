#include "wifi.h"
#include "../os/clock.h"
#include "../os/config.h"
#include "display.h"
#include "http.h"

#include "mongoose.h"
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

// ── SNTP
// ──────────────────────────────────────────────────────────────────────

static void sntp_cb(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_SNTP_TIME) {
    int64_t *t = (int64_t *)ev_data;
    printf("WiFi: SNTP sync OK, time: %lld\n", *t);
    clock_sntp_set((unsigned)(*t / 1000));
    c->is_closing = 1;
    if (!s_http_required) {
      // Don't call wifi_disconnect() directly here — we're inside mg_mgr_poll()
      // and calling mg_wifi_disconnect() from within a Mongoose callback causes
      // reentrancy into the CYW43 driver.  Set a flag; wifi_poll() will
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

// ── Public API
// ─────────────────────────────────────────────────────────────────

void wifi_init(void) {
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

  s_driver_data.wifi.ssid = s_ssid;
  s_driver_data.wifi.pass = s_pass;

  printf("WiFi: connecting to '%s'...\n", s_ssid);

  mg_wifi_connect(&s_driver_data.wifi);
}

void wifi_disconnect(void) {
  if (!s_available)
    return;

  mg_wifi_disconnect();

  s_status = WIFI_STATUS_DISCONNECTED;
  s_ssid[0] = '\0';
  s_ip[0] = '\0';
  printf("WiFi: disconnected\n");
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

  // Skip polling when disconnected to save power
  wifi_status_t st = wifi_get_status();
  if (st != WIFI_STATUS_CONNECTED && st != WIFI_STATUS_CONNECTING)
    return;

  mg_mgr_poll(&s_mgr, 0);

  // Process any disconnect deferred from inside a Mongoose callback (SNTP etc.)
  if (s_disconnect_pending) {
    s_disconnect_pending = false;
    wifi_disconnect();
  }
}

// Access to manager for HTTP
struct mg_mgr *wifi_get_mgr(void) { return &s_mgr; }

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
