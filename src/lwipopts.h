#pragma once

// =============================================================================
// lwipopts.h — lwIP configuration for PicOS (pico_cyw43_arch_lwip_poll mode)
//
// Required by lwIP when built via pico_cyw43_arch_lwip_poll.
// Using NO_SYS=1 + polling mode: cyw43_arch_poll() is called from the Lua
// instruction hook instead of a background thread.
// =============================================================================

// No RTOS — cooperative polling via cyw43_arch_poll()
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// Memory — use libc malloc in polling mode (safe: single-threaded Core 0)
#define MEM_LIBC_MALLOC             1
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000

// Increase timer pool to prevent "MEMP_SYS_TIMEOUT is empty" panics
#define MEMP_NUM_SYS_TIMEOUT        12

// Buffer pools
#define MEMP_NUM_TCP_SEG            16
#define MEMP_NUM_ARP_QUEUE          5
#define PBUF_POOL_SIZE              16

// Protocols
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1

// TCP tuning — modest values for embedded use
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

// Callbacks used by cyw43_arch
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

// DHCP — skip ARP check (saves time on connect)
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// Checksum algorithm (3 = optimised for 32-bit ARM)
#define LWIP_CHKSUM_ALGORITHM       3

// TLS support via mbedTLS
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1

// Disable stats to save flash/RAM
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0

// Turn off all debug output (saves flash)
#define LWIP_DEBUG                  0

// SNTP — fetch UTC from pool.ntp.org once WiFi connects
extern void clock_sntp_set(unsigned sec);  // forward decl (clock.h not included here)
#define SNTP_SERVER_DNS             1
#define SNTP_SERVER_ADDRESS         "pool.ntp.org"
#define SNTP_SET_SYSTEM_TIME(sec)   clock_sntp_set((unsigned)(sec))
