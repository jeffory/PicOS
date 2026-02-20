#include "wifi.h"
#include "display.h"
#include "../os/config.h"

#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

#ifdef WIFI_ENABLED
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/apps/sntp.h"
#include "mbedtls/platform_time.h"

// mbedTLS 3.x requires mbedtls_ms_time() when MBEDTLS_HAVE_TIME is defined.
// The Pico SDK does not provide one, so we supply it here using the Pico's
// hardware microsecond counter.
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(time_us_64() / 1000);
}
#endif

// ── State ─────────────────────────────────────────────────────────────────────

static bool          s_available = false;
static wifi_status_t s_status    = WIFI_STATUS_DISCONNECTED;
static char          s_ssid[64]  = {0};
static char          s_ip[20]    = {0};

// ── Internal helpers ──────────────────────────────────────────────────────────

#ifdef WIFI_ENABLED
// Map CYW43 link state to our wifi_status_t.
static wifi_status_t cyw43_link_to_status(int link) {
    if (link == CYW43_LINK_UP)                     return WIFI_STATUS_CONNECTED;
    if (link == CYW43_LINK_JOIN || link == CYW43_LINK_NOIP) return WIFI_STATUS_CONNECTING;
    if (link < 0)                                  return WIFI_STATUS_FAILED;
    return WIFI_STATUS_DISCONNECTED;
}
#endif

// ── Public API ─────────────────────────────────────────────────────────────────

void wifi_init(void) {
#ifdef WIFI_ENABLED
    display_spi_lock();
    int err = cyw43_arch_init();
    if (err == 0) {
        cyw43_arch_enable_sta_mode();
        s_available = true;
    }
    display_spi_unlock();

    if (!s_available) {
        printf("WiFi: cyw43_arch_init failed (%d) — no WiFi hardware\n", err);
        return;
    }
    printf("WiFi: CYW43 ready\n");

    // Auto-connect if credentials are stored in /system/config.json.
    const char *ssid = config_get("wifi_ssid");
    const char *pass = config_get("wifi_pass");
    if (ssid && ssid[0]) {
        printf("WiFi: auto-connecting to '%s'\n", ssid);
        wifi_connect(ssid, pass ? pass : "");
    }
#else
    s_available = false;
    printf("WiFi: not compiled in (WIFI_ENABLED not set)\n");
#endif
}

bool wifi_is_available(void) {
    return s_available;
}

void wifi_connect(const char *ssid, const char *password) {
    if (!s_available || !ssid || !ssid[0]) return;

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    s_status = WIFI_STATUS_CONNECTING;
    s_ip[0]  = '\0';

#ifdef WIFI_ENABLED
    display_spi_lock();
    // WPA2_MIXED_PSK handles WPA and WPA2 networks.
    int err = cyw43_arch_wifi_connect_async(s_ssid, password,
                                            CYW43_AUTH_WPA2_MIXED_PSK);
    display_spi_unlock();

    if (err != 0) {
        s_status = WIFI_STATUS_FAILED;
        printf("WiFi: connect_async failed (%d)\n", err);
    } else {
        printf("WiFi: connecting to '%s'\n", s_ssid);
    }
#endif
}

void wifi_disconnect(void) {
    if (!s_available) return;

#ifdef WIFI_ENABLED
    display_spi_lock();
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    display_spi_unlock();
#endif

    s_status = WIFI_STATUS_DISCONNECTED;
    s_ssid[0] = '\0';
    s_ip[0]   = '\0';
    printf("WiFi: disconnected\n");
}

wifi_status_t wifi_get_status(void) {
    return s_status;
}

const char *wifi_get_ip(void) {
    if (s_status != WIFI_STATUS_CONNECTED) return NULL;
    return s_ip[0] ? s_ip : NULL;
}

const char *wifi_get_ssid(void) {
    return s_ssid[0] ? s_ssid : NULL;
}

void wifi_poll(void) {
    if (!s_available) return;

#ifdef WIFI_ENABLED
    display_spi_lock();
    cyw43_arch_poll();

    // Only update status when actively connecting or connected.
    if (s_status == WIFI_STATUS_CONNECTING || s_status == WIFI_STATUS_CONNECTED) {
        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        wifi_status_t new_status = cyw43_link_to_status(link);

        if (new_status != s_status) {
            s_status = new_status;

            if (s_status == WIFI_STATUS_CONNECTED) {
                // Capture IP address from lwip netif
                if (netif_default) {
                    ip4addr_ntoa_r(netif_ip4_addr(netif_default),
                                   s_ip, sizeof(s_ip));
                }
                printf("WiFi: connected  IP=%s\n", s_ip);
                sntp_init();
            } else if (s_status == WIFI_STATUS_FAILED) {
                printf("WiFi: connect failed (link=%d)\n", link);
                sntp_stop();
            } else if (s_status == WIFI_STATUS_DISCONNECTED) {
                s_ip[0] = '\0';
                printf("WiFi: link lost\n");
                sntp_stop();
            }
        }
    }

    display_spi_unlock();
#endif
}
