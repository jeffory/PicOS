/* Minimal newlib syscall stubs for bare-metal native app.
   Only snprintf is used (no file I/O, no heap). */

#include <sys/stat.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

int _close(int file) { (void)file; return -1; }
int _lseek(int file, int ptr, int dir) { (void)file; (void)ptr; (void)dir; return 0; }
int _read(int file, char *ptr, int len) { (void)file; (void)ptr; (void)len; return 0; }
int _write(int file, char *ptr, int len) { (void)file; (void)ptr; return len; }
int _fstat(int file, struct stat *st) { (void)file; st->st_mode = S_IFCHR; return 0; }
int _isatty(int file) { (void)file; return 1; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status) { (void)status; while (1) {} }

void *_sbrk(ptrdiff_t incr) {
    (void)incr;
    errno = ENOMEM;
    return (void *)-1;
}
