#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include "os.h"

extern const PicoCalcAPI *g_picos_api;
extern char g_app_dir[128];

// --- Exit recovery ---
// Defined in dg_picos.c — longjmp target so _exit() returns to picos_main()
// instead of spinning forever.
extern jmp_buf g_exit_jmp;

// --- Heap for malloc/sbrk ---
// 2.5 MB: zone memory (~2 MB) + overhead for other malloc calls.
// This lives in BSS and inflates the ELF's p_memsz — keep it as small
// as practical so the OS ELF loader's PSRAM allocation succeeds.
#define HEAP_SIZE (2560 * 1024)
static uint8_t g_heap[HEAP_SIZE] __attribute__((aligned(8)));
static uint8_t *g_heap_ptr = g_heap;

void * _sbrk(ptrdiff_t incr) {
    uint8_t *prev_ptr = g_heap_ptr;
    if (g_heap_ptr + incr > g_heap + HEAP_SIZE) {
        errno = ENOMEM;
        return (void *)-1;
    }
    g_heap_ptr += incr;
    return prev_ptr;
}

// --- Serial output buffering ---
// sys->log() appends '\n' to every call, so we buffer output and flush
// on newline or when the buffer is full.  Without this, each character
// from printf() would appear on its own line.
static char s_log_buf[256];
static int  s_log_pos = 0;

static void log_flush(void) {
    if (s_log_pos > 0) {
        s_log_buf[s_log_pos] = '\0';
        g_picos_api->sys->log(s_log_buf);
        s_log_pos = 0;
    }
}

// --- File System Stubs (mapped to PicOS FS API) ---

static pcfile_t g_fd_table[16] = {0};

int _open(const char *name, int flags, int mode) {
    char full_path[256];
    if (name[0] != '/') {
        snprintf(full_path, sizeof(full_path), "%s/%s", g_app_dir, name);
    } else {
        strncpy(full_path, name, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    }

    const char *picos_mode = "rb";
    if ((flags & 0x3) == 1) picos_mode = "wb";
    else if ((flags & 0x3) == 2) picos_mode = "w+b";

    for (int i = 0; i < 16; i++) {
        if (g_fd_table[i] == NULL) {
            g_fd_table[i] = g_picos_api->fs->open(full_path, picos_mode);
            if (g_fd_table[i]) return i + 3;
            return -1;
        }
    }
    return -1;
}

int _read(int file, char *ptr, int len) {
    if (file < 3) return 0;
    pcfile_t f = g_fd_table[file - 3];
    if (!f) return -1;
    return g_picos_api->fs->read(f, ptr, len);
}

int _write(int file, char *ptr, int len) {
    if (file == 1 || file == 2) {
        for (int i = 0; i < len; i++) {
            if (ptr[i] == '\n' || s_log_pos >= (int)sizeof(s_log_buf) - 1) {
                log_flush();
            } else {
                s_log_buf[s_log_pos++] = ptr[i];
            }
        }
        return len;
    }
    if (file < 3) return -1;
    pcfile_t f = g_fd_table[file - 3];
    if (!f) return -1;
    return g_picos_api->fs->write(f, ptr, len);
}

int _close(int file) {
    if (file < 3) return 0;
    pcfile_t f = g_fd_table[file - 3];
    if (!f) return -1;
    g_picos_api->fs->close(f);
    g_fd_table[file - 3] = NULL;
    return 0;
}

int _lseek(int file, int ptr, int dir) {
    if (file < 3) return 0;
    pcfile_t f = g_fd_table[file - 3];
    if (!f) return -1;
    uint32_t target = ptr;
    if (dir == 1) target = g_picos_api->fs->tell(f) + ptr;
    else if (dir == 2) target = g_picos_api->fs->fsize(f) + ptr;
    g_picos_api->fs->seek(f, target);
    return g_picos_api->fs->tell(f);
}

int _fstat(int file, struct stat *st) {
    st->st_mode = S_IFREG;
    if (file < 3) st->st_mode = S_IFCHR;
    st->st_size = (file >= 3 && g_fd_table[file-3]) ? g_picos_api->fs->fsize(g_fd_table[file-3]) : 0;
    return 0;
}

int _isatty(int file) {
    if (file < 3) return 1;
    return 0;
}

int _unlink(const char *name) { return -1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { return -1; }

void _exit(int status) {
    // Flush any buffered log output before leaving.
    log_flush();
    // Jump back to picos_main's setjmp so the app returns cleanly to the
    // launcher instead of spinning forever.
    longjmp(g_exit_jmp, status ? status : -1);
    // longjmp never returns, but the compiler needs this:
    __builtin_unreachable();
}

int mkdir(const char *path, mode_t mode) { return 0; }
int _link(const char *old, const char *new) { return -1; }

// --- Networking / Joystick Stubs ---

typedef int boolean;
#ifndef false
#define false 0
#endif
#ifndef true
#define true 1
#endif

// These are needed because we excluded dummy.c
boolean net_client_connected = false;
boolean drone = false;

// Joystick is in i_joystick.c which we excluded
void I_InitJoystick(void) {}
void I_BindJoystickVariables(void) {}
