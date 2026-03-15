#include "dev_commands.h"
#include "drivers/display.h"
#include "tusb.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define CMD_BUF_SIZE 64

static char s_cmd_buf[CMD_BUF_SIZE];
static size_t s_cmd_len = 0;
static bool s_cmd_ready = false;

static bool s_cmd_exit = false;
static bool s_cmd_usb = false;
static bool s_cmd_reboot = false;
static bool s_cmd_reboot_flash = false;
static bool s_cmd_list = false;
static const char* s_pending_launch = NULL;

void dev_commands_init(void) {
    s_cmd_buf[0] = '\0';
    s_cmd_len = 0;
    s_cmd_ready = false;
    s_cmd_exit = false;
    s_cmd_usb = false;
    s_cmd_reboot = false;
    s_cmd_reboot_flash = false;
    s_cmd_list = false;
    s_pending_launch = NULL;
}

const char* dev_commands_get_device(void) {
    const char* dev = getenv("SERIAL_DEVICE");
    return dev ? dev : "/dev/ttyACM0";
}

void dev_commands_poll(void) {
    // Read through pico_stdio (which owns the CDC endpoint), not tud_cdc_read
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) break;

        if (c == '\n' || c == '\r') {
            if (s_cmd_len > 0) {
                s_cmd_buf[s_cmd_len] = '\0';
                s_cmd_ready = true;
                s_cmd_len = 0;
            }
        } else if (c == 0x03) {
            s_cmd_len = 0;
            s_cmd_buf[0] = '\0';
        } else if (s_cmd_len < CMD_BUF_SIZE - 1) {
            s_cmd_buf[s_cmd_len++] = (char)c;
            s_cmd_buf[s_cmd_len] = '\0';
        }
    }
}

// Write binary data over CDC, bypassing stdio (for screenshot bulk transfer).
// Caller must call stdio_flush() / fflush(stdout) before this.
static void cdc_write_all(const uint8_t *data, uint32_t len) {
    while (len > 0) {
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            tud_cdc_write_flush();
            tud_task();
            continue;
        }
        uint32_t chunk = (len < avail) ? len : avail;
        uint32_t written = tud_cdc_write(data, chunk);
        data += written;
        len -= written;
    }
    tud_cdc_write_flush();
}

void dev_commands_send_screenshot(void) {
    const uint16_t *fb = display_get_screen_buffer();

    // Header: "SCRN" + width(u16 LE) + height(u16 LE) + format(u16 LE) + pad(2)
    uint8_t header[12];
    header[0] = 'S'; header[1] = 'C'; header[2] = 'R'; header[3] = 'N';
    uint16_t w = FB_WIDTH, h = FB_HEIGHT, fmt = 565;
    memcpy(&header[4], &w, 2);
    memcpy(&header[6], &h, 2);
    memcpy(&header[8], &fmt, 2);
    header[10] = 0; header[11] = 0;

    // Flush stdio so our raw CDC writes don't interleave with printf output
    stdio_flush();

    cdc_write_all(header, sizeof(header));
    cdc_write_all((const uint8_t *)fb, FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
}

bool dev_commands_process(void) {
    if (!s_cmd_ready) return false;

    printf("[DEV] Command: %s\n", s_cmd_buf);

    if (strcmp(s_cmd_buf, "ping") == 0) {
        printf("[DEV] pong\n");
    } else if (strcmp(s_cmd_buf, "exit") == 0) {
        s_cmd_exit = true;
    } else if (strcmp(s_cmd_buf, "usb") == 0) {
        s_cmd_usb = true;
    } else if (strcmp(s_cmd_buf, "reboot") == 0) {
        s_cmd_reboot = true;
    } else if (strcmp(s_cmd_buf, "reboot-flash") == 0) {
        s_cmd_reboot_flash = true;
    } else if (strncmp(s_cmd_buf, "launch ", 7) == 0) {
        s_pending_launch = s_cmd_buf + 7;
        s_cmd_exit = true;  // Exit current app first
    } else if (strcmp(s_cmd_buf, "list") == 0) {
        s_cmd_list = true;
    } else if (strcmp(s_cmd_buf, "screenshot") == 0) {
        dev_commands_send_screenshot();
    } else {
        printf("[DEV] Unknown command: %s\n", s_cmd_buf);
    }

    s_cmd_buf[0] = '\0';
    s_cmd_ready = false;
    return true;
}

bool dev_commands_wants_exit(void) {
    return s_cmd_exit;
}

void dev_commands_clear_exit(void) {
    s_cmd_exit = false;
}

bool dev_commands_wants_usb(void) {
    return s_cmd_usb;
}

void dev_commands_clear_usb(void) {
    s_cmd_usb = false;
}

bool dev_commands_wants_reboot(void) {
    return s_cmd_reboot;
}

bool dev_commands_wants_reboot_flash(void) {
    return s_cmd_reboot_flash;
}

bool dev_commands_wants_list(void) {
    return s_cmd_list;
}

void dev_commands_clear_list(void) {
    s_cmd_list = false;
}

const char* dev_commands_get_pending_launch(void) {
    return s_pending_launch;
}

void dev_commands_clear_pending_launch(void) {
    s_pending_launch = NULL;
}
