/*
 * Minimal newlib syscall stubs for bare-metal PIE apps.
 * The C64 emulator uses the PicOS API directly, so most of these are no-ops.
 * They're only needed to satisfy newlib's libc/libm references.
 */
#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

// Small heap for any internal libc needs (snprintf buffers, etc.)
#define HEAP_SIZE (16 * 1024)
static uint8_t g_heap[HEAP_SIZE] __attribute__((aligned(8)));
static uint8_t *g_heap_ptr = g_heap;

void *_sbrk(ptrdiff_t incr) {
    uint8_t *prev = g_heap_ptr;
    if (g_heap_ptr + incr > g_heap + HEAP_SIZE) {
        errno = ENOMEM;
        return (void *)-1;
    }
    g_heap_ptr += incr;
    return prev;
}

int _close(int file) { return -1; }
int _lseek(int file, int ptr, int dir) { return 0; }
int _read(int file, char *ptr, int len) { return 0; }
int _write(int file, char *ptr, int len) { return len; }
int _fstat(int file, struct stat *st) { st->st_mode = S_IFCHR; return 0; }
int _isatty(int file) { return 1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { return -1; }
void _exit(int status) { while (1) {} }
