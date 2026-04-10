/*
  DOS86 — BIOS INT 13h disk services for fake86 on PicOS.
  Geometry auto-detected from image size. Read/write via backend_disk_*.
*/

#ifndef FAKE86_DISK_H
#define FAKE86_DISK_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum number of emulated drives (A:, B:, C:, D:) */
#define DISK_MAX 4

/* Per-drive geometry, detected when image is mounted */
typedef struct {
    uint16_t cyls;
    uint16_t heads;
    uint16_t sects;       /* sectors per track */
    uint32_t size_bytes;
    bool     inserted;
    bool     is_hdd;      /* true if drive >= 0x80 */
} disk_geo_t;

/* Mount a disk image and auto-detect geometry.
   drive: 0x00=A:, 0x01=B:, 0x80=C:, 0x81=D: */
bool disk_mount(uint8_t drive, const char *path);

/* Mount with explicit file size (for geometry detection) */
bool disk_mount_with_size(uint8_t drive, const char *path, uint32_t file_size);

/* Unmount a drive */
void disk_unmount(uint8_t drive);

/* Check if a drive is mounted */
bool disk_is_mounted(uint8_t drive);

/* Get geometry for a mounted drive (NULL if not mounted) */
const disk_geo_t *disk_get_geo(uint8_t drive);

/* INT 13h BIOS disk handler — called from intcall86 for INT 0x13 */
void diskhandler(void);

/* INT 19h bootstrap — read boot sector and jump to it */
void disk_bootstrap(void);

/* Read sectors directly (used by bootstrap).
   Reads sectcount sectors starting at CHS into dstseg:dstoff. */
void disk_read_chs(uint8_t drive, uint16_t dstseg, uint16_t dstoff,
                   uint16_t cyl, uint16_t sect, uint16_t head,
                   uint16_t sectcount);

#endif /* FAKE86_DISK_H */
