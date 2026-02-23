#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "../os/os.h"   // wifi_status_t

// =============================================================================
// WiFi Driver — CYW43 on Pimoroni Pico Plus 2W
//
// SPI1 is shared between the LCD and the CYW43 WiFi chip. All CYW43
// operations are serialised behind display_spi_lock() / display_spi_unlock()
// to prevent bus conflicts with LCD DMA transfers.
//
// Connection is non-blocking: call wifi_connect(), then poll wifi_get_status()
// or let the OS Lua hook drive wifi_poll() automatically in the background.
//
// Compile guard: WIFI_ENABLED=1 is defined by CMakeLists when the board has
// a CYW43 chip. All functions are safe no-ops when WIFI_ENABLED is absent.
// =============================================================================

// Initialise CYW43 hardware and enable station mode. Call once during boot
// after sdcard / config are initialised. Sets wifi_is_available() if hardware
// is found. If config holds "wifi_ssid" / "wifi_pass", starts auto-connect.
void wifi_init(void);

// Returns true if CYW43 hardware was found and initialised successfully.
bool wifi_is_available(void);

// Begin connecting to a WiFi network (non-blocking, WPA/WPA2).
// s_status transitions to WIFI_STATUS_CONNECTING immediately; check
// wifi_get_status() for CONNECTED or FAILED.
void wifi_connect(const char *ssid, const char *password);

// Disconnect from the current network.
void wifi_disconnect(void);

// Current connection state (WIFI_STATUS_*).
wifi_status_t wifi_get_status(void);

// Null-terminated IP address string (e.g. "192.168.1.42"), or NULL if not
// connected.
const char *wifi_get_ip(void);

// SSID of the current or pending connection, or NULL if fully disconnected.
const char *wifi_get_ssid(void);

// Drive the CYW43 lwip-poll state machine and update connection status.
// Must be called regularly. The OS Lua instruction hook calls this every
// ~256 opcodes so apps do not need to call it themselves.
// No-op when WiFi hardware is not available.
void wifi_poll(void);

// Set whether an app with HTTP requirement is running. When true, WiFi will
// not automatically disconnect after SNTP time sync.
void wifi_set_http_required(bool required);
bool wifi_get_http_required(void);
