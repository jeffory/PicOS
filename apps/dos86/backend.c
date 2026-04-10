/*
  DOS86 — PicOS backend: timing, keyboard buffer, disk I/O.
  Uses PicOS API (api->fs->*, api->sys->*) for hardware access.
*/

#include "backend.h"
#include "os.h"
#include "scancode.h"
#include "fake86/i8259.h"
#include <string.h>

/* ---- Module state ---- */

static const PicoCalcAPI *s_api = NULL;

/* ---- Keyboard ring buffer ---- */

#define KB_BUF_SIZE 16
static uint8_t  s_kb_buf[KB_BUF_SIZE];
static int      s_kb_head = 0;  /* next slot to read */
static int      s_kb_tail = 0;  /* next slot to write */

/* ---- Disk drives ---- */

#define DISK_MAX_DRIVES 4

typedef struct {
    pcfile_t f;
    uint32_t size_bytes;
    bool     mounted;
} disk_drive_t;

static disk_drive_t s_drives[DISK_MAX_DRIVES];

/* Map drive number to internal index.
   0x00=A:(idx 0), 0x01=B:(idx 1), 0x80=C:(idx 2), 0x81=D:(idx 3) */
static int drive_idx(int drive) {
    if (drive == 0x00) return 0;
    if (drive == 0x01) return 1;
    if (drive == 0x80) return 2;
    if (drive == 0x81) return 3;
    return -1;
}

/* ---- Init / shutdown ---- */

void backend_init(const struct PicoCalcAPI *api, const char *app_dir) {
    s_api = api;
    (void)app_dir;

    s_kb_head = 0;
    s_kb_tail = 0;
    memset(s_kb_buf, 0, sizeof(s_kb_buf));
    memset(s_drives, 0, sizeof(s_drives));
}

void backend_shutdown(void) {
    for (int i = 0; i < DISK_MAX_DRIVES; i++) {
        if (s_drives[i].mounted && s_drives[i].f) {
            s_api->fs->close(s_drives[i].f);
        }
        s_drives[i].mounted = false;
        s_drives[i].f = NULL;
    }
    s_api = NULL;
}

/* ---- Timing ---- */

uint64_t backend_get_timer_ticks(void) {
    return s_api->sys->getTimeUs();
}

uint32_t backend_get_ms(void) {
    return s_api->sys->getTimeMs();
}

/* ---- Keyboard buffer ---- */

void backend_kb_push(uint8_t scancode) {
    int next = (s_kb_tail + 1) % KB_BUF_SIZE;
    if (next == s_kb_head) {
        /* Buffer full: drop oldest byte */
        s_kb_head = (s_kb_head + 1) % KB_BUF_SIZE;
    }
    s_kb_buf[s_kb_tail] = scancode;
    s_kb_tail = next;
}

uint8_t backend_kb_read(void) {
    if (s_kb_head == s_kb_tail) {
        return 0;  /* Buffer empty */
    }
    uint8_t val = s_kb_buf[s_kb_head];
    s_kb_head = (s_kb_head + 1) % KB_BUF_SIZE;
    return val;
}

bool backend_kb_available(void) {
    return s_kb_head != s_kb_tail;
}

static char s_last_char;

void backend_pump_keyboard(void) {
    uint32_t pressed  = s_api->input->getButtonsPressed();
    uint32_t released = s_api->input->getButtonsReleased();

    /* Button-mapped keys */
    for (int i = 0; btn_scancode_table[i].btn_mask != 0; i++) {
        uint32_t mask = btn_scancode_table[i].btn_mask;
        uint8_t  sc   = btn_scancode_table[i].scancode;
        if (pressed & mask) {
            backend_kb_push(sc);          /* Make code */
            i8259_irq(1);                 /* Keyboard IRQ1 */
        }
        if (released & mask) {
            backend_kb_push(sc | 0x80);   /* Break code */
            i8259_irq(1);
        }
    }

    /* ASCII character keys */
    char c = s_api->input->getChar();
    if (c && c != s_last_char) {
        uint8_t sc = ascii_to_scancode(c);
        if (sc) {
            backend_kb_push(sc);
            i8259_irq(1);
        }
        s_last_char = c;
    } else if (!c && s_last_char) {
        uint8_t sc = ascii_to_scancode(s_last_char);
        if (sc) {
            backend_kb_push(sc | 0x80);
            i8259_irq(1);
        }
        s_last_char = 0;
    }
}

/* ---- Disk I/O ---- */

bool backend_disk_mount(int drive, const char *path) {
    int idx = drive_idx(drive);
    if (idx < 0) return false;

    /* Unmount existing if mounted */
    if (s_drives[idx].mounted && s_drives[idx].f) {
        s_api->fs->close(s_drives[idx].f);
    }

    pcfile_t f = s_api->fs->open(path, "rb");
    if (!f) return false;

    s_drives[idx].f = f;
    s_drives[idx].size_bytes = (uint32_t)s_api->fs->fsize(f);
    s_drives[idx].mounted = true;
    return true;
}

void backend_disk_unmount(int drive) {
    int idx = drive_idx(drive);
    if (idx < 0) return;

    if (s_drives[idx].mounted && s_drives[idx].f) {
        s_api->fs->close(s_drives[idx].f);
    }
    s_drives[idx].f = NULL;
    s_drives[idx].mounted = false;
    s_drives[idx].size_bytes = 0;
}

int backend_disk_read(int drive, uint32_t lba, uint8_t *buf, int count) {
    int idx = drive_idx(drive);
    if (idx < 0 || !s_drives[idx].mounted || !s_drives[idx].f) return -1;

    uint32_t offset = lba * 512u;
    if (!s_api->fs->seek(s_drives[idx].f, offset)) return -1;

    int bytes_to_read = count * 512;
    int got = s_api->fs->read(s_drives[idx].f, buf, bytes_to_read);
    if (got < 0) return -1;
    return got / 512;
}

int backend_disk_write(int drive, uint32_t lba, const uint8_t *buf, int count) {
    int idx = drive_idx(drive);
    if (idx < 0 || !s_drives[idx].mounted || !s_drives[idx].f) return -1;

    uint32_t offset = lba * 512u;
    if (!s_api->fs->seek(s_drives[idx].f, offset)) return -1;

    int bytes_to_write = count * 512;
    int written = s_api->fs->write(s_drives[idx].f, buf, bytes_to_write);
    if (written < 0) return -1;
    return written / 512;
}

bool backend_disk_mounted(int drive) {
    int idx = drive_idx(drive);
    if (idx < 0) return false;
    return s_drives[idx].mounted;
}

uint32_t backend_disk_get_size(int drive) {
    int idx = drive_idx(drive);
    if (idx < 0 || !s_drives[idx].mounted) return 0;
    return s_drives[idx].size_bytes;
}

/* ---- Display ---- */

void backend_render_frame(const uint16_t *framebuf, int width, int height) {
    /* Stub: display rendering implemented in Task 7 */
    (void)framebuf;
    (void)width;
    (void)height;
}
