#include "usb_msc.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../drivers/wifi.h"
#include "../os/config.h"
#include "../os/os.h"
#include "../os/ui.h"

// Define BYTE/LBA_t and other FatFs types manually before diskio.h
// just in case they are missing from diskio.h inclusion order
#include <stdint.h>
#include <stdio.h>
typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef uint32_t DWORD;
typedef DWORD LBA_t;

#include "diskio.h"
#include "ff.h"

#include <string.h>

// Block devices and filesystems
static uint16_t msc_block_size = 512;
static bool s_msc_active = false;

// --------------------------------------------------------------------
// USB MSC Entry point
// --------------------------------------------------------------------

void usb_msc_enter_mode(void) {
  printf("[USB MSC] Entering USB Mass Storage mode\n");

  // 1. Disconnect WiFi to prevent SPI contention with USB
  bool was_connected = (wifi_get_status() == WIFI_STATUS_CONNECTED);
  if (was_connected) {
    printf("[USB MSC] Disconnecting WiFi...\n");
    wifi_disconnect();
  }

  // 2. Unmount FatFS so host can take over the SD card safely
  printf("[USB MSC] Unmounting FatFS...\n");
  f_unmount("");
  s_msc_active = true;

  // NOTE: tusb_init() is already called by pico_stdio_usb during
  // stdio_init_all(). Our custom tusb_config.h and usb_descriptors.c
  // configure it as a composite CDC+MSC device from boot. We do NOT
  // call tusb_init() again here — doing so could corrupt the stack.
  //
  // The MSC interface is always present in the descriptor but the
  // callbacks return "not ready" while s_msc_active is false.

  printf("[USB MSC] TinyUSB connected=%d, mounted=%d\n", tud_connected(),
         tud_mounted());

  // 2. Draw the splash screen
  ui_draw_splash("USB Mode", "Press ESC to exit");

  // 3. Poll loop — tud_task() is also called by the SDK's background IRQ,
  //    but calling it here too ensures responsive MSC handling.
  printf("[USB MSC] Waiting for host or ESC key...\n");
  
  uint32_t poll_loop_ms = 0;
  uint32_t last_kbd_poll_ms = 0;
  const uint32_t KBD_POLL_INTERVAL_MS = 10;  // Poll keyboard every 10ms max
  const uint32_t HOST_TIMEOUT_MS = 5000;      // 5 second timeout if no host activity
  
  while (true) {
    // Service USB, giving it priority
    uint32_t poll_start = to_ms_since_boot(get_absolute_time());
    tud_task();
    
    // Service WiFi (if connected) to prevent lwIP timeouts
    wifi_poll();
    
    // Check ESC key with rate limiting to avoid I2C bus congestion
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_kbd_poll_ms >= KBD_POLL_INTERVAL_MS) {
      kbd_poll();
      last_kbd_poll_ms = now;
      if (kbd_get_buttons_pressed() & BTN_ESC) {
        printf("[USB MSC] ESC key pressed, exiting\n");
        break;
      }
    }
    
    // Check if host is still connected
    if (!tud_mounted() && poll_loop_ms > HOST_TIMEOUT_MS) {
      printf("[USB MSC] Host disconnected for >%ums, exiting\n", HOST_TIMEOUT_MS);
      break;
    }
    
    // Track loop timing for timeout detection
    uint32_t poll_end = to_ms_since_boot(get_absolute_time());
    poll_loop_ms += (poll_end - poll_start);
    if (poll_loop_ms > HOST_TIMEOUT_MS) {
      poll_loop_ms = 0;  // Reset counter each 5 seconds
    }
    
    sleep_us(100); // 100µs base interval
  }

  // 4. Deactivate MSC and remount
  //    Do NOT call tud_disconnect() — that would kill CDC serial too.
  //    Just set s_msc_active=false so callbacks return "not ready" again.
  printf("[USB MSC] Exiting USB Mass Storage mode\n");
  s_msc_active = false;

  // Remount FatFS
  printf("[USB MSC] Remounting FatFS...\n");
  sdcard_remount();

  // Reconnect WiFi if we were connected before
  if (was_connected) {
    const char *ssid = config_get("wifi_ssid");
    const char *pass = config_get("wifi_pass");
    if (ssid && ssid[0]) {
      printf("[USB MSC] Reconnecting WiFi...\n");
      wifi_connect(ssid, pass ? pass : "");
    }
  }

  printf("[USB MSC] Done\n");
}

// --------------------------------------------------------------------
// USB MSC callbacks
// --------------------------------------------------------------------

// Invoked when device is mounted by the host
void tud_mount_cb(void) { printf("[USB MSC] Device mounted by host\n"); }

// Invoked when device is unmounted by the host
void tud_umount_cb(void) { printf("[USB MSC] Device unmounted by host\n"); }

// Invoked to determine max LUN
uint8_t tud_msc_get_maxlun_cb(void) { return 0; }

void tud_msc_inquiry_cb(uint8_t lun, uint8_t p_vendor_id[8],
                        uint8_t p_product_id[16], uint8_t p_product_rev[4]) {
  (void)lun;
  static const char vendor[8] = "PICO";
  static const char product[16] = "PicOS_MSC";
  static const char revision[4] = "1.0 ";

  memcpy(p_vendor_id, vendor, sizeof(vendor));
  memcpy(p_product_id, product, sizeof(product));
  memcpy(p_product_rev, revision, sizeof(revision));
  printf("[USB MSC] Inquiry callback\n");
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
  (void)lun;
  LBA_t count = 0;

  if (s_msc_active && disk_ioctl(0, GET_SECTOR_COUNT, &count) == RES_OK &&
      count > 0) {
    *block_size = msc_block_size;
    *block_count = (uint32_t)count;
    printf("[USB MSC] Capacity: %lu blocks x %u bytes\n", (unsigned long)count,
           msc_block_size);
  } else {
    *block_size = 0;
    *block_count = 0;
    printf("[USB MSC] Capacity: not ready (active=%d)\n", s_msc_active);
  }
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject) {
  (void)lun;
  (void)power_condition;
  printf("[USB MSC] Start/Stop: start=%d, load_eject=%d\n", start, load_eject);
  return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
  (void)lun;
  (void)offset;

  if (!s_msc_active)
    return -1;

  if (disk_read(0, (BYTE *)buffer, lba, bufsize / msc_block_size) != RES_OK) {
    printf("[USB MSC] Read error at LBA %lu\n", (unsigned long)lba);
    return -1;
  }

  return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return s_msc_active;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
  (void)lun;
  (void)offset;

  if (!s_msc_active)
    return -1;

  if (disk_write(0, (const BYTE *)buffer, lba, bufsize / msc_block_size) !=
      RES_OK) {
    printf("[USB MSC] Write error at LBA %lu\n", (unsigned long)lba);
    return -1;
  }

  return (int32_t)bufsize;
}

void tud_msc_write10_flush_cb(uint8_t lun) {
  (void)lun;
  disk_ioctl(0, CTRL_SYNC, NULL);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  // Only check s_msc_active — sdcard_is_mounted() tracks FatFS mount
  // state, which is intentionally false during MSC mode.
  if (!s_msc_active) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return false;
  }
  return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize) {
  (void)buffer;
  (void)bufsize;

  switch (scsi_cmd[0]) {
  default:
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
  }
}
