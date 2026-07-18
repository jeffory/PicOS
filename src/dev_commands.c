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

// Base64 receive mode (UART-safe alternative to the CDC binary path).
// Chars arrive via stdio, so this works on any transport.  The host sends
// newline-terminated chunks of base64 (multiple of 4 chars); each chunk is
// decoded, flushed to SD, and acknowledged so the sender can pace itself —
// the UART RX FIFO is only 32 bytes and SD write latency can exceed it.
#define B64_RECV_TIMEOUT_US (10u * 1000u * 1000u)
#define B64_WRITE_BUF_SIZE 512
static bool s_b64_recv_active = false;
static char s_b64_group[4];
static uint32_t s_b64_group_len = 0;
static uint8_t s_b64_writebuf[B64_WRITE_BUF_SIZE];
static uint32_t s_b64_writelen = 0;
static uint32_t s_b64_hash = 2166136261u; // FNV-1a running hash
static uint64_t s_b64_last_rx_us = 0;

// FNV-1a 32-bit — cheap integrity check for serial transfers (not
// cryptographic; OTA does its own SHA-256 before touching flash).
static inline uint32_t fnv1a_update(uint32_t h, const uint8_t *p, uint32_t n) {
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static const char s_b64_enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void b64_recv_char(int c);
static void b64_recv_abort(const char *why);

// Encode exactly n (1-3) bytes into 4 chars.
static void b64_encode_group(const uint8_t *in, uint32_t n, char out[4]) {
    uint32_t v = (uint32_t)in[0] << 16;
    if (n > 1) v |= (uint32_t)in[1] << 8;
    if (n > 2) v |= in[2];
    out[0] = s_b64_enc[(v >> 18) & 63];
    out[1] = s_b64_enc[(v >> 12) & 63];
    out[2] = (n > 1) ? s_b64_enc[(v >> 6) & 63] : '=';
    out[3] = (n > 2) ? s_b64_enc[v & 63] : '=';
}

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
    s_b64_recv_active = false;
    s_b64_group_len = 0;
    s_b64_writelen = 0;
}

const char* dev_commands_get_device(void) {
    const char* dev = getenv("SERIAL_DEVICE");
    return dev ? dev : "/dev/ttyACM0";
}

void dev_commands_poll(void) {
    if (s_file_recv_handle && s_b64_recv_active) {
        while (true) {
            int c = getchar_timeout_us(0);
            if (c == PICO_ERROR_TIMEOUT) break;
            b64_recv_char(c);
            if (!s_b64_recv_active) return;
        }
        if (time_us_64() - s_b64_last_rx_us > B64_RECV_TIMEOUT_US)
            b64_recv_abort("timeout");
        return;
    }

    if (s_file_recv_handle) {
        uint8_t buf[FILE_RECEIVE_CHUNK_SIZE];
        while (s_file_recv_received < s_file_recv_expected) {
            uint32_t got = tud_cdc_read(buf, FILE_RECEIVE_CHUNK_SIZE);
            if (got == 0) break;
            s_b64_last_rx_us = time_us_64();
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
        } else if (time_us_64() - s_b64_last_rx_us > B64_RECV_TIMEOUT_US) {
            // Stalled CDC transfer (e.g. host went away): abort so the console
            // does not stay captured in receive mode forever.
            sdcard_fclose(s_file_recv_handle);
            s_file_recv_handle = NULL;
            printf("[DEV] Error: put timed out at %lu/%lu bytes\n",
                   (unsigned long)s_file_recv_received,
                   (unsigned long)s_file_recv_expected);
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
// Returns false if CDC is not mounted or the host stops draining: spinning
// unconditionally here starves the watchdog and resets the device when the
// only connection is UART (no CDC host to drain the FIFO).
static bool cdc_write_all(const uint8_t *data, uint32_t len) {
    if (!tud_cdc_connected()) return false;
    uint64_t last_progress_us = time_us_64();
    while (len > 0) {
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            tud_cdc_write_flush();
            tud_task();
            if (time_us_64() - last_progress_us > 2000000u) return false;
            continue;
        }
        uint32_t chunk = (len < avail) ? len : avail;
        uint32_t written = tud_cdc_write(data, chunk);
        data += written;
        len -= written;
        if (written > 0) {
            last_progress_us = time_us_64();
            watchdog_update();
        }
    }
    tud_cdc_write_flush();
    return true;
}

static void b64_recv_abort(const char *why) {
    if (s_file_recv_handle) {
        sdcard_fclose(s_file_recv_handle);
        s_file_recv_handle = NULL;
    }
    s_b64_recv_active = false;
    printf("[DEV] Error: b64 receive aborted (%s) at %lu/%lu bytes\n",
           why, (unsigned long)s_file_recv_received,
           (unsigned long)s_file_recv_expected);
}

static void b64_recv_flush(void) {
    if (s_b64_writelen == 0) return;
    int written = sdcard_fwrite(s_file_recv_handle, s_b64_writebuf, s_b64_writelen);
    if (written < 0) {
        b64_recv_abort("SD write failed");
        return;
    }
    s_b64_hash = fnv1a_update(s_b64_hash, s_b64_writebuf, s_b64_writelen);
    s_file_recv_received += (uint32_t)written;
    s_b64_writelen = 0;
}

static void b64_recv_char(int c) {
    s_b64_last_rx_us = time_us_64();
    if (c == 0x03) { // Ctrl-C abort from host
        b64_recv_abort("host abort");
        return;
    }
    if (c == '\n' || c == '\r') {
        if (c == '\n') {
            b64_recv_flush();
            if (!s_b64_recv_active) return; // flush may have aborted
            if (s_file_recv_received >= s_file_recv_expected) {
                sdcard_fclose(s_file_recv_handle);
                s_file_recv_handle = NULL;
                s_b64_recv_active = false;
                printf("[DEV] File received: %s (%lu bytes) fnv1a=%08lx\n",
                       s_file_recv_path, (unsigned long)s_file_recv_received,
                       (unsigned long)s_b64_hash);
            } else {
                printf("[DEV] ACK %lu\n", (unsigned long)s_file_recv_received);
            }
        }
        return;
    }
    if (c == '=') { // padding: decode the final partial group
        // "xx==" → 1 byte (handled on first '='; second '=' sees group_len 0),
        // "xxx=" → 2 bytes.
        if (s_b64_group_len >= 2 && s_b64_writelen + 2 <= B64_WRITE_BUF_SIZE) {
            uint32_t v = ((uint32_t)b64_decode_char(s_b64_group[0]) << 18) |
                         ((uint32_t)b64_decode_char(s_b64_group[1]) << 12);
            s_b64_writebuf[s_b64_writelen++] = (uint8_t)(v >> 16);
            if (s_b64_group_len == 3) {
                v |= (uint32_t)b64_decode_char(s_b64_group[2]) << 6;
                s_b64_writebuf[s_b64_writelen++] = (uint8_t)(v >> 8);
            }
        }
        s_b64_group_len = 0;
        return;
    }
    int v = b64_decode_char((char)c);
    if (v < 0) {
        b64_recv_abort("invalid base64 char");
        return;
    }
    s_b64_group[s_b64_group_len++] = (char)c;
    if (s_b64_group_len == 4) {
        s_b64_group_len = 0;
        uint32_t g = ((uint32_t)b64_decode_char(s_b64_group[0]) << 18) |
                     ((uint32_t)b64_decode_char(s_b64_group[1]) << 12) |
                     ((uint32_t)b64_decode_char(s_b64_group[2]) << 6) |
                     (uint32_t)b64_decode_char(s_b64_group[3]);
        if (s_b64_writelen + 3 > B64_WRITE_BUF_SIZE) {
            b64_recv_abort("chunk exceeds buffer");
            return;
        }
        s_b64_writebuf[s_b64_writelen++] = (uint8_t)(g >> 16);
        s_b64_writebuf[s_b64_writelen++] = (uint8_t)(g >> 8);
        s_b64_writebuf[s_b64_writelen++] = (uint8_t)g;
    }
}

void dev_commands_send_screenshot(void) {
    if (!tud_cdc_connected()) {
        printf("[DEV] Error: CDC not connected — use screenshot64\n");
        return;
    }
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

    if (!cdc_write_all(header, sizeof(header)) ||
        !cdc_write_all((const uint8_t *)fb, FB_WIDTH * FB_HEIGHT * sizeof(uint16_t)))
        printf("[DEV] Error: CDC write stalled\n");
}

// Stream a memory buffer as '~'-prefixed base64 lines over stdio.  The '~'
// prefix lets the host discard any log lines Core 1 interleaves into the
// stream; the FNV-1a in the caller's footer catches mid-line interleaving.
#define B64_LINE_RAW 72 // 96 base64 chars per line
static uint32_t b64_send_buf(const uint8_t *data, uint32_t len, uint32_t hash) {
    char line[(B64_LINE_RAW / 3) * 4 + 1];
    for (uint32_t off = 0; off < len; off += B64_LINE_RAW) {
        uint32_t n = len - off < B64_LINE_RAW ? len - off : B64_LINE_RAW;
        uint32_t li = 0;
        for (uint32_t i = 0; i < n; i += 3) {
            uint32_t g = n - i < 3 ? n - i : 3;
            b64_encode_group(data + off + i, g, &line[li]);
            li += 4;
        }
        line[li] = '\0';
        printf("~%s\n", line);
        hash = fnv1a_update(hash, data + off, n);
        watchdog_update();
    }
    return hash;
}

// Stream a file as base64 over stdio (any transport).  Single pass: the
// FNV-1a of the raw bytes is only known at the end, so it rides the footer.
static void dev_send_file_b64(const char *path) {
    sdfile_t f = sdcard_fopen(path, "rb");
    if (!f) {
        printf("[DEV] Failed to open file: %s\n", path);
        return;
    }
    int size = sdcard_fsize_handle(f);
    printf("[DEV] B64 size=%d\n", size);
    uint8_t buf[B64_LINE_RAW * 4];
    uint32_t hash = 2166136261u;
    int nread;
    while ((nread = sdcard_fread(f, buf, sizeof(buf))) > 0)
        hash = b64_send_buf(buf, (uint32_t)nread, hash);
    sdcard_fclose(f);
    printf("[DEV] B64_END fnv1a=%08lx path=%s\n", (unsigned long)hash, path);
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
        s_b64_last_rx_us = time_us_64();
        printf("[DEV] Ready to receive %lu bytes for %s\n", (unsigned long)size, args);
    } else if (strncmp(s_cmd_buf, "putb64 ", 7) == 0) {
        char *args = s_cmd_buf + 7;
        uint32_t size = 0;
        char *size_str = strchr(args, ' ');
        if (size_str) {
            *size_str = '\0';
            size = atoi(size_str + 1);
        }
        if (size == 0 || strlen(args) == 0) {
            printf("[DEV] Usage: putb64 <path> <raw_size>\n");
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
        s_b64_recv_active = true;
        s_b64_group_len = 0;
        s_b64_writelen = 0;
        s_b64_hash = 2166136261u;
        s_b64_last_rx_us = time_us_64();
        printf("[DEV] Ready B64 %lu bytes for %s (chunk<=%u raw, newline-terminated, await ACK)\n",
               (unsigned long)size, args, (unsigned)B64_WRITE_BUF_SIZE);
    } else if (strncmp(s_cmd_buf, "getb64 ", 7) == 0) {
        dev_send_file_b64(s_cmd_buf + 7);
    } else if (strcmp(s_cmd_buf, "screenshot64") == 0) {
        const uint8_t *fb = (const uint8_t *)display_get_screen_buffer();
        printf("[DEV] SCRN64 w=%u h=%u fmt=565\n", (unsigned)FB_WIDTH, (unsigned)FB_HEIGHT);
        uint32_t hash = b64_send_buf(fb, FB_WIDTH * FB_HEIGHT * 2u, 2166136261u);
        printf("[DEV] SCRN64_END fnv1a=%08lx\n", (unsigned long)hash);
    } else if (strcmp(s_cmd_buf, "crashlog") == 0) {
        sdfile_t f = sdcard_fopen("/system/crashlog.txt", "rb");
        if (!f) {
            printf("[DEV] No crash log\n");
        } else {
            printf("[DEV] CRASHLOG BEGIN\n");
            char buf[257];
            int nread;
            while ((nread = sdcard_fread(f, buf, sizeof(buf) - 1)) > 0) {
                buf[nread] = '\0';
                printf("%s", buf);
                watchdog_update();
            }
            sdcard_fclose(f);
            printf("\n[DEV] CRASHLOG END\n");
        }
    } else if (strcmp(s_cmd_buf, "crashlog clear") == 0) {
        sdcard_delete("/system/crashlog.txt");
        printf("[DEV] Crash log cleared\n");
    } else if (strncmp(s_cmd_buf, "get ", 4) == 0) {
        const char *path = s_cmd_buf + 4;
        if (strlen(path) == 0) {
            printf("[DEV] Usage: get <path>\n");
            s_cmd_buf[0] = '\0';
            s_cmd_ready = false;
            return true;
        }
        if (!tud_cdc_connected()) {
            printf("[DEV] Error: CDC not connected — use getb64\n");
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
            if (!cdc_write_all(buf, read)) {
                printf("[DEV] Error: CDC write stalled\n");
                break;
            }
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
        printf("[DEV]   put <path> <size> - Receive file from host (USB CDC only)\n");
        printf("[DEV]   get <path>     - Send file to host (USB CDC only)\n");
        printf("[DEV]   putb64 <path> <size> - Receive file as base64 (any transport)\n");
        printf("[DEV]   getb64 <path>  - Send file as base64 (any transport)\n");
        printf("[DEV]   screenshot64   - Capture screen as base64 (any transport)\n");
        printf("[DEV]   crashlog       - Print /system/crashlog.txt ('crashlog clear' deletes)\n");
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
