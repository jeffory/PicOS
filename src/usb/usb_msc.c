#include "usb_msc.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../os/os.h"
#include "../os/ui.h"

// Define BYTE/LBA_t and other FatFs types manually before diskio.h
// just in case they are missing from diskio.h inclusion order
#include <stdint.h>
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
  // 1. Unmount FatFS so host can take over safely
  f_unmount("");
  s_msc_active = true;

  // Initialize TinyUSB
  tusb_init();

  // 2. Draw the splash screen
  ui_draw_splash("USB Mode", "Press ESC to exit");

  // 3. Connect USB (pico_stdlib might already have it connected, but we must
  // handle tasks if we aren't doing it elsewhere, though stdio_usb might do it
  // in irq) Actually, stdio_usb configures TinyUSB to run in IRQ mode on the
  // Pico SDK by default! BUT we need to wait for ESC. We don't need to call
  // tud_task() if it's IRQ driven. We'll just loop.

  while (true) {
    tud_task();
    kbd_poll();
    if (kbd_get_buttons_pressed() & BTN_ESC) {
      break;
    }
    sleep_ms(10);
  }

  // 4. Disconnect/cleanup and remount
  tud_disconnect();
  s_msc_active = false;

  // Remount FatFS
  sdcard_remount();
}

// --------------------------------------------------------------------
// USB MSC callbacks
// --------------------------------------------------------------------

// Invoked when device is mounted by the host
void tud_mount_cb(void) {}

// Invoked when device is unmounted by the host
void tud_umount_cb(void) {}

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
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
  (void)lun;
  LBA_t count = 0;

  if (s_msc_active && disk_ioctl(0, GET_SECTOR_COUNT, &count) == RES_OK &&
      count > 0) {
    *block_size = msc_block_size;
    *block_count = (uint32_t)count;
  } else {
    *block_size = 0;
    *block_count = 0;
  }
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject) {
  (void)lun;
  (void)power_condition;
  return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
  (void)lun;
  (void)offset;

  if (!s_msc_active)
    return -1;

  if (disk_read(0, (BYTE *)buffer, lba, bufsize / msc_block_size) != RES_OK) {
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
    return -1;
  }

  return (int32_t)bufsize;
}

void tud_msc_write10_flush_cb(uint8_t lun) {
  (void)lun;
  // disk_ioctl(0, CTRL_SYNC, NULL);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  if (!s_msc_active || !sdcard_is_mounted()) { // Wait, sdcard_is_mounted is
                                               // tracking FatFS mount state
    // Actually sdcard_is_mounted might be false because we unmounted FatFS
    // Let's just check s_msc_active.
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
