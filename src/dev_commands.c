#include "dev_commands.h"
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
static bool s_cmd_list = false;
static const char* s_pending_launch = NULL;

void dev_commands_init(void) {
    s_cmd_buf[0] = '\0';
    s_cmd_len = 0;
    s_cmd_ready = false;
    s_cmd_exit = false;
    s_cmd_usb = false;
    s_cmd_reboot = false;
    s_cmd_list = false;
    s_pending_launch = NULL;
}

const char* dev_commands_get_device(void) {
    const char* dev = getenv("SERIAL_DEVICE");
    return dev ? dev : "/dev/ttyACM0";
}

void dev_commands_poll(void) {
    if (!tud_cdc_connected()) return;
    
    uint8_t buf[32];
    uint32_t count = tud_cdc_read(buf, sizeof(buf));
    if (count == 0) return;
    
    for (uint32_t i = 0; i < count; i++) {
        char c = buf[i];
        
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
            s_cmd_buf[s_cmd_len++] = c;
            s_cmd_buf[s_cmd_len] = '\0';
        }
    }
}

bool dev_commands_process(void) {
    if (!s_cmd_ready) return false;
    
    printf("[DEV] Command: %s\n", s_cmd_buf);
    
    if (strcmp(s_cmd_buf, "exit") == 0) {
        s_cmd_exit = true;
    } else if (strcmp(s_cmd_buf, "usb") == 0) {
        s_cmd_usb = true;
    } else if (strcmp(s_cmd_buf, "reboot") == 0) {
        s_cmd_reboot = true;
    } else if (strncmp(s_cmd_buf, "launch ", 7) == 0) {
        s_pending_launch = s_cmd_buf + 7;
        s_cmd_exit = true;  // Exit current app first
    } else if (strcmp(s_cmd_buf, "list") == 0) {
        s_cmd_list = true;
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
