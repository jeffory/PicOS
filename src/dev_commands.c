#include "dev_commands.h"
#include "drivers/display.h"
#include "drivers/keyboard.h"
#include "drivers/sdcard.h"
#include "os/os.h"
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

#define FILE_RECEIVE_CHUNK_SIZE 256
static char s_file_recv_path[128];
static sdfile_t s_file_recv_handle = NULL;
static uint32_t s_file_recv_expected = 0;
static uint32_t s_file_recv_received = 0;

static void dev_ls_callback(const sdcard_entry_t *entry, void *user) {
    (void)user;
    if (entry->name[0] == '.') return;
    if (entry->is_dir) {
        printf("  %s/\n", entry->name);
    } else {
        printf("  %s  (%u bytes)\n", entry->name, entry->size);
    }
}

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
    s_file_recv_handle = NULL;
    s_file_recv_expected = 0;
    s_file_recv_received = 0;
}

const char* dev_commands_get_device(void) {
    const char* dev = getenv("SERIAL_DEVICE");
    return dev ? dev : "/dev/ttyACM0";
}

void dev_commands_poll(void) {
    if (s_file_recv_handle) {
        uint8_t buf[FILE_RECEIVE_CHUNK_SIZE];
        while (s_file_recv_received < s_file_recv_expected) {
            uint32_t got = tud_cdc_read(buf, FILE_RECEIVE_CHUNK_SIZE);
            if (got == 0) break;
            int written = sdcard_fwrite(s_file_recv_handle, buf, got);
            if (written < 0) {
                printf("[DEV] Error writing file\n");
                sdcard_fclose(s_file_recv_handle);
                s_file_recv_handle = NULL;
                return;
            }
            s_file_recv_received += written;
        }
        if (s_file_recv_received >= s_file_recv_expected) {
            sdcard_fclose(s_file_recv_handle);
            s_file_recv_handle = NULL;
            printf("[DEV] File received: %s (%lu bytes)\n", s_file_recv_path, (unsigned long)s_file_recv_received);
        }
        return;
    }

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
    } else if (strncmp(s_cmd_buf, "keypress ", 9) == 0) {
        const char *key = s_cmd_buf + 9;
        uint32_t buttons = 0;
        char ch = 0;

        if (strcmp(key, "up") == 0)       buttons = BTN_UP;
        else if (strcmp(key, "down") == 0)     buttons = BTN_DOWN;
        else if (strcmp(key, "left") == 0)    buttons = BTN_LEFT;
        else if (strcmp(key, "right") == 0)   buttons = BTN_RIGHT;
        else if (strcmp(key, "enter") == 0)    buttons = BTN_ENTER;
        else if (strcmp(key, "esc") == 0)      buttons = BTN_ESC;
        else if (strcmp(key, "menu") == 0)     buttons = BTN_MENU;
        else if (strcmp(key, "backspace") == 0) buttons = BTN_BACKSPACE;
        else if (strcmp(key, "tab") == 0)       buttons = BTN_TAB;
        else if (strcmp(key, "del") == 0)      buttons = BTN_DEL;
        else if (strcmp(key, "shift") == 0)    buttons = BTN_SHIFT;
        else if (strcmp(key, "f1") == 0)       buttons = BTN_F1;
        else if (strcmp(key, "f2") == 0)       buttons = BTN_F2;
        else if (strcmp(key, "f3") == 0)       buttons = BTN_F3;
        else if (strcmp(key, "f4") == 0)       buttons = BTN_F4;
        else if (strcmp(key, "f5") == 0)       buttons = BTN_F5;
        else if (strcmp(key, "f6") == 0)       buttons = BTN_F6;
        else if (strcmp(key, "f7") == 0)       buttons = BTN_F7;
        else if (strcmp(key, "f8") == 0)       buttons = BTN_F8;
        else if (strcmp(key, "f9") == 0)       buttons = BTN_F9;
        else if (strcmp(key, "f10") == 0)      buttons = (1 << 15);
        else if (strlen(key) == 1 && key[0] >= 0x20 && key[0] < 0x7F) {
            ch = key[0];
        } else {
            printf("[DEV] Unknown key: %s\n", key);
            s_cmd_buf[0] = '\0';
            s_cmd_ready = false;
            return true;
        }

        if (buttons) kbd_inject_buttons(buttons);
        if (ch) kbd_inject_char(ch);
        printf("[DEV] Key injected: %s\n", key);
    } else if (strncmp(s_cmd_buf, "put ", 4) == 0) {
        const char *args = s_cmd_buf + 4;
        uint32_t size = 0;
        char *size_str = strchr(args, ' ');
        if (size_str) {
            *size_str = '\0';
            size = atoi(size_str + 1);
        }
        if (size == 0 || strlen(args) == 0) {
            printf("[DEV] Usage: put <path> <size>\n");
            s_cmd_buf[0] = '\0';
            s_cmd_ready = false;
            return true;
        }
        strncpy(s_file_recv_path, args, sizeof(s_file_recv_path) - 1);
        s_file_recv_path[sizeof(s_file_recv_path) - 1] = '\0';
        s_file_recv_handle = sdcard_fopen(s_file_recv_path, "wb");
        if (!s_file_recv_handle) {
            printf("[DEV] Failed to open file for writing: %s\n", args);
            s_cmd_buf[0] = '\0';
            s_cmd_ready = false;
            return true;
        }
        s_file_recv_expected = size;
        s_file_recv_received = 0;
        printf("[DEV] Ready to receive %lu bytes for %s\n", (unsigned long)size, args);
    } else if (strncmp(s_cmd_buf, "get ", 4) == 0) {
        const char *path = s_cmd_buf + 4;
        if (strlen(path) == 0) {
            printf("[DEV] Usage: get <path>\n");
            s_cmd_buf[0] = '\0';
            s_cmd_ready = false;
            return true;
        }
        sdfile_t f = sdcard_fopen(path, "rb");
        if (!f) {
            printf("[DEV] Failed to open file: %s\n", path);
            s_cmd_buf[0] = '\0';
            s_cmd_ready = false;
            return true;
        }
        int size = sdcard_fsize_handle(f);
        printf("FILE_DATA:\nSIZE:%d\n", size);
        stdio_flush();

        uint8_t buf[256];
        int read;
        while ((read = sdcard_fread(f, buf, sizeof(buf))) > 0) {
            cdc_write_all(buf, read);
        }
        sdcard_fclose(f);
        printf("[DEV] File sent: %s (%d bytes)\n", path, size);
    } else if (strncmp(s_cmd_buf, "ls ", 3) == 0) {
        const char *path = s_cmd_buf + 3;
        if (strlen(path) == 0) {
            path = "/";
        }
        int count = sdcard_list_dir(path, dev_ls_callback, NULL);
        printf("[DEV] %d items in %s\n", count < 0 ? 0 : count, path);
    } else if (strcmp(s_cmd_buf, "help") == 0) {
        printf("[DEV] Available commands:\n");
        printf("[DEV]   ping           - Check device is responding\n");
        printf("[DEV]   exit           - Signal current app to exit\n");
        printf("[DEV]   usb            - Enable USB storage mode\n");
        printf("[DEV]   reboot         - Reboot device\n");
        printf("[DEV]   reboot-flash   - Reboot to BOOTSEL for flashing\n");
        printf("[DEV]   launch <arg>   - Launch app by ID or name\n");
        printf("[DEV]   list           - List installed apps\n");
        printf("[DEV]   screenshot     - Capture screen\n");
        printf("[DEV]   keypress <key> - Inject keypress\n");
        printf("[DEV]   put <path> <size> - Receive file from host\n");
        printf("[DEV]   get <path>     - Send file to host\n");
        printf("[DEV]   ls <dir>       - List directory contents\n");
        printf("[DEV]   help           - Show this help\n");
        printf("[DEV] Valid keys: up, down, left, right, enter, esc, menu, f1-f10, backspace, tab, del, shift, a-z, A-Z, 0-9, punctuation\n");
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

void dev_commands_set_exit(void) {
    s_cmd_exit = true;
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
