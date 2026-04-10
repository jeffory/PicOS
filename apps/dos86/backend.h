#pragma once
#include <stdint.h>
#include <stdbool.h>

struct PicoCalcAPI;

void backend_init(const struct PicoCalcAPI *api, const char *app_dir);
void backend_shutdown(void);

/* Timing */
uint64_t backend_get_timer_ticks(void);
uint32_t backend_get_ms(void);

/* Keyboard buffer (ring buffer) */
void    backend_kb_push(uint8_t scancode);
uint8_t backend_kb_read(void);
bool    backend_kb_available(void);
void    backend_pump_keyboard(void);  /* stub for now, implemented in Task 6 */

/* Disk I/O */
bool backend_disk_mount(int drive, const char *path);
void backend_disk_unmount(int drive);
int  backend_disk_read(int drive, uint32_t lba, uint8_t *buf, int count);
int  backend_disk_write(int drive, uint32_t lba, const uint8_t *buf, int count);
bool backend_disk_mounted(int drive);
uint32_t backend_disk_get_size(int drive);

/* Display (stub for now, implemented in Task 7) */
void backend_render_frame(const uint16_t *framebuf, int width, int height);
