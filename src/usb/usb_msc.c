#include "usb_msc.h"
#include "hardware/watchdog.h"
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
static volatile bool s_msc_ejected = false;  // Set by tud_msc_start_stop_cb when host ejects

// --------------------------------------------------------------------
// USB MSC Entry point
// --------------------------------------------------------------------

// Declared in main.c - pauses Core 1's background tasks (WiFi, HTTP, audio)
extern volatile bool g_core1_pause;
extern volatile bool g_core1_paused;

void usb_msc_enter_mode(void) {
  printf("[USB MSC] Entering USB Mass Storage mode\n");

  // 1. Disconnect WiFi and pause Core 1 to prevent SPI/I2C contention
  //    Core 1 runs WiFi/HTTP/audio tasks every 5ms which can interfere
  //    with USB MSC operations and cause keyboard I2C timeouts.
  bool was_connected = (wifi_get_status() == WIFI_STATUS_CONNECTED);
  if (was_connected) {
    printf("[USB MSC] Disconnecting WiFi...\n");
    wifi_disconnect();
  }
  
  // Pause Core 1 completely - wait for acknowledgment to ensure it's stopped
  g_core1_pause = true;
  while (!g_core1_paused) {
    sleep_ms(1);
  }
  printf("[USB MSC] Core 1 paused (WiFi/HTTP/audio halted)\n");

  // 2. Unmount FatFS so host can take over the SD card safely
  printf("[USB MSC] Unmounting FatFS...\n");
  f_unmount("");
  s_msc_active = true;
  s_msc_ejected = false;  // Reset eject flag on entry

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
  ui_draw_splash("USB Mode", "Hold escape to exit");

  // 3. Poll loop — USB events are processed exclusively by the SDK's
  //    USBCTRL_IRQ background task (PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=1
  //    installs low_priority_worker_irq which calls tud_task() under
  //    stdio_usb_mutex).  We must NOT call tud_task() here: the IRQ and a
  //    direct call share no mutual exclusion around the processing callbacks,
  //    so both could call disk_read() simultaneously, corrupting SPI0 state
  //    and hanging the device (which manifests as the keyboard locking up).
  // CRITICAL: I2C keyboard polling is limited to 500ms intervals during USB MSC.
  // uf2loader-main reference shows that frequent I2C access during active USB
  // causes STM32 lockup due to electrical interference/noise. This slower rate
  // balances usability (ESC key still works) with stability (prevents crashes).
  // The "Hold escape" message cues users to keep the key pressed longer.
  // See: reference/uf2loader-main/ui/text_directory_ui.c (no I2C during USB)
  printf("[USB MSC] Waiting for host or ESC key (hold to exit)...\n");

  uint32_t last_kbd_poll_ms = 0;
  uint32_t loop_start_ms = to_ms_since_boot(get_absolute_time());
  const uint32_t KBD_POLL_INTERVAL_MS = 500;  // Poll keyboard every 500ms (was 10ms)
  const uint32_t HOST_TIMEOUT_MS = 5000;      // 5 second timeout if no host activity

  while (true) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Check ESC key with rate limiting to avoid I2C bus congestion
    if (now - last_kbd_poll_ms >= KBD_POLL_INTERVAL_MS) {
      kbd_poll();
      last_kbd_poll_ms = now;
      if (kbd_get_buttons_pressed() & BTN_ESC) {
        printf("[USB MSC] ESC key pressed, exiting\n");
        break;
      }
    }

    // Check for ESC via CDC serial (for automated workflows)
    int cdc_ch = getchar_timeout_us(0);
    if (cdc_ch != PICO_ERROR_TIMEOUT) {
      if (cdc_ch == 'x' || cdc_ch == 'X' || cdc_ch == 0x03) {
        printf("[USB MSC] ESC via CDC, exiting\n");
        break;
      }
    }

    // Exit if host has ejected the device
    if (s_msc_ejected) {
      printf("[USB MSC] Device ejected by host, exiting\n");
      break;
    }

    // Exit if no host has mounted the device within HOST_TIMEOUT_MS.
    // Use wall time so the timeout is accurate regardless of loop body duration.
    if (!tud_mounted() && (now - loop_start_ms) > HOST_TIMEOUT_MS) {
      printf("[USB MSC] No host connected for >%ums, exiting\n", HOST_TIMEOUT_MS);
      break;
    }

    sleep_us(100); // 100µs base interval — lets USB IRQs fire between iterations
    watchdog_update(); // keep watchdog alive while in USB mode
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

  // Recover I2C bus - STM32 may need re-initialization after USB activity
  printf("[USB MSC] Recovering I2C bus...\n");
  kbd_recover_i2c_bus();
  kbd_apply_clock();
  kbd_clear_state();

  printf("[USB MSC] Done\n");

  // Resume Core 1 background tasks
  g_core1_pause = false;
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

  if (s_msc_active) {
    recursive_mutex_enter_blocking(&g_sdcard_mutex);
    DRESULT res = disk_ioctl(0, GET_SECTOR_COUNT, &count);
    recursive_mutex_exit(&g_sdcard_mutex);
    
    if (res == RES_OK && count > 0) {
      *block_size = msc_block_size;
      *block_count = (uint32_t)count;
      printf("[USB MSC] Capacity: %lu blocks x %u bytes\n", (unsigned long)count,
             msc_block_size);
    } else {
      *block_size = 0;
      *block_count = 0;
      printf("[USB MSC] Capacity: not ready (res=%d, count=%lu)\n", res, (unsigned long)count);
    }
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
  (void)start;
  if (load_eject) {
    printf("[USB MSC] Host ejected device\n");
    s_msc_ejected = true;
  }
  return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
  (void)lun;
  (void)offset;

  if (!s_msc_active)
    return -1;

  recursive_mutex_enter_blocking(&g_sdcard_mutex);
  DRESULT res = disk_read(0, (BYTE *)buffer, lba, bufsize / msc_block_size);
  recursive_mutex_exit(&g_sdcard_mutex);
  
  if (res != RES_OK) {
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

  recursive_mutex_enter_blocking(&g_sdcard_mutex);
  DRESULT res = disk_write(0, (const BYTE *)buffer, lba, bufsize / msc_block_size);
  recursive_mutex_exit(&g_sdcard_mutex);
  
  if (res != RES_OK) {
    printf("[USB MSC] Write error at LBA %lu\n", (unsigned long)lba);
    return -1;
  }

  return (int32_t)bufsize;
}

void tud_msc_write10_flush_cb(uint8_t lun) {
  (void)lun;
  recursive_mutex_enter_blocking(&g_sdcard_mutex);
  disk_ioctl(0, CTRL_SYNC, NULL);
  recursive_mutex_exit(&g_sdcard_mutex);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  // Only check s_msc_active — sdcard_is_mounted() tracks FatFS mount
  // state, which is intentionally false during MSC mode.
  // Note: No mutex needed here as we don't access SPI0, just check a flag.
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
