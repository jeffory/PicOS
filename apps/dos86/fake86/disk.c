/*
  DOS86 — BIOS INT 13h disk services for fake86 on PicOS.
  Copyright (C)2010-2013 Mike Chambers (original fake86 disk.c)
  Adapted for PicOS by dos86 project.

  Geometry is auto-detected from image file size.
  Actual I/O goes through backend_disk_read/write (PicOS fs API).
*/

#include "disk.h"
#include "cpu.h"
#include "config.h"
#include "../backend.h"
#include <string.h>

/* ---- Per-drive state ---- */

static disk_geo_t s_geo[DISK_MAX];
static uint8_t    s_hdcount = 0;

/* Sector buffer for read/write (one sector at a time through emulated RAM) */
static uint8_t s_sectorbuf[512];

/* Last status per drive (for AH=01h) */
static uint8_t s_last_ah[DISK_MAX];
static uint8_t s_last_cf[DISK_MAX];

/* Map BIOS drive number to internal index */
static int drive_to_idx(uint8_t drive) {
    if (drive == 0x00) return 0;
    if (drive == 0x01) return 1;
    if (drive == 0x80) return 2;
    if (drive == 0x81) return 3;
    return -1;
}

/* ---- Geometry detection ---- */

static void detect_geometry(disk_geo_t *g, uint32_t size, bool is_hdd) {
    g->size_bytes = size;
    g->is_hdd = is_hdd;

    if (!is_hdd) {
        /* Floppy geometry detection */
        g->cyls = 80;
        g->heads = 2;
        g->sects = 18;

        if (size <= 1228800) g->sects = 15;
        if (size <= 737280)  g->sects = 9;
        if (size <= 368640) {
            g->cyls = 40;
            g->sects = 9;
        }
        if (size <= 163840) {
            g->cyls = 40;
            g->sects = 8;
            g->heads = 1;
        }
    } else {
        /* Hard disk geometry */
        g->heads = 16;
        g->sects = 63;
        g->cyls = size / ((uint32_t)g->sects * g->heads * 512);
        if (g->cyls == 0) g->cyls = 1;
    }
}

/* ---- Public API ---- */

bool disk_mount(uint8_t drive, const char *path) {
    int idx = drive_to_idx(drive);
    if (idx < 0) return false;

    /* Unmount existing */
    if (s_geo[idx].inserted) {
        backend_disk_unmount(drive);
        s_geo[idx].inserted = false;
        if (s_geo[idx].is_hdd && s_hdcount > 0) s_hdcount--;
    }

    /* Mount via backend (which records file size) */
    if (!backend_disk_mount(drive, path)) return false;

    /* Get size from backend and detect geometry */
    uint32_t file_size = backend_disk_get_size(drive);
    bool is_hdd = (drive >= 0x80);
    detect_geometry(&s_geo[idx], file_size, is_hdd);
    s_geo[idx].inserted = true;
    s_last_ah[idx] = 0;
    s_last_cf[idx] = 0;

    if (is_hdd) s_hdcount++;

    return true;
}

/* Mount with explicit file size for geometry detection */
bool disk_mount_with_size(uint8_t drive, const char *path, uint32_t file_size) {
    int idx = drive_to_idx(drive);
    if (idx < 0) return false;

    /* Unmount existing */
    if (s_geo[idx].inserted) {
        backend_disk_unmount(drive);
        s_geo[idx].inserted = false;
        if (s_geo[idx].is_hdd) s_hdcount--;
    }

    /* Mount via backend */
    if (!backend_disk_mount(drive, path)) return false;

    bool is_hdd = (drive >= 0x80);
    detect_geometry(&s_geo[idx], file_size, is_hdd);
    s_geo[idx].inserted = true;
    s_last_ah[idx] = 0;
    s_last_cf[idx] = 0;

    if (is_hdd) s_hdcount++;

    return true;
}

void disk_unmount(uint8_t drive) {
    int idx = drive_to_idx(drive);
    if (idx < 0) return;

    if (s_geo[idx].inserted) {
        backend_disk_unmount(drive);
        if (s_geo[idx].is_hdd && s_hdcount > 0) s_hdcount--;
    }
    memset(&s_geo[idx], 0, sizeof(disk_geo_t));
}

bool disk_is_mounted(uint8_t drive) {
    int idx = drive_to_idx(drive);
    if (idx < 0) return false;
    return s_geo[idx].inserted;
}

const disk_geo_t *disk_get_geo(uint8_t drive) {
    int idx = drive_to_idx(drive);
    if (idx < 0 || !s_geo[idx].inserted) return NULL;
    return &s_geo[idx];
}

/* ---- Internal sector read/write through emulated RAM ---- */

static void read_sectors(uint8_t drive, uint16_t dstseg, uint16_t dstoff,
                         uint16_t cyl, uint16_t sect, uint16_t head,
                         uint16_t sectcount)
{
    int idx = drive_to_idx(drive);
    if (idx < 0 || !s_geo[idx].inserted || sect == 0) return;

    uint32_t lba = ((uint32_t)cyl * s_geo[idx].heads + head) *
                   s_geo[idx].sects + sect - 1;
    uint32_t memdest = ((uint32_t)dstseg << 4) + dstoff;
    uint16_t cursect;

    for (cursect = 0; cursect < sectcount; cursect++) {
        int got = backend_disk_read(drive, lba + cursect, s_sectorbuf, 1);
        if (got < 1) break;

        /* Copy sector into emulated RAM via write86 (honors ROM protection) */
        for (int i = 0; i < 512; i++) {
            write86(memdest++, s_sectorbuf[i]);
        }
    }

    regs.byteregs[regal] = (uint8_t)cursect;
    cf = 0;
    regs.byteregs[regah] = 0;
}

static void write_sectors(uint8_t drive, uint16_t dstseg, uint16_t dstoff,
                          uint16_t cyl, uint16_t sect, uint16_t head,
                          uint16_t sectcount)
{
    int idx = drive_to_idx(drive);
    if (idx < 0 || !s_geo[idx].inserted || sect == 0) return;

    uint32_t lba = ((uint32_t)cyl * s_geo[idx].heads + head) *
                   s_geo[idx].sects + sect - 1;
    uint32_t memsrc = ((uint32_t)dstseg << 4) + dstoff;
    uint16_t cursect;

    for (cursect = 0; cursect < sectcount; cursect++) {
        /* Read sector from emulated RAM */
        for (int i = 0; i < 512; i++) {
            s_sectorbuf[i] = read86(memsrc++);
        }

        int wrote = backend_disk_write(drive, lba + cursect, s_sectorbuf, 1);
        if (wrote < 1) break;
    }

    regs.byteregs[regal] = (uint8_t)cursect;
    cf = 0;
    regs.byteregs[regah] = 0;
}

/* ---- INT 13h handler ---- */

void diskhandler(void) {
    uint8_t drive = regs.byteregs[regdl];
    int idx = drive_to_idx(drive);

    switch (regs.byteregs[regah]) {
        case 0x00: /* Reset disk system */
            regs.byteregs[regah] = 0;
            cf = 0;
            break;

        case 0x01: /* Return last status */
            if (idx >= 0) {
                regs.byteregs[regah] = s_last_ah[idx];
                cf = s_last_cf[idx];
            } else {
                regs.byteregs[regah] = 0x01;
                cf = 1;
            }
            return; /* Don't update last status for this call */

        case 0x02: /* Read sectors */
            if (idx >= 0 && s_geo[idx].inserted) {
                uint16_t cyl = regs.byteregs[regch] +
                               ((uint16_t)(regs.byteregs[regcl] >> 6) << 8);
                uint16_t sect = regs.byteregs[regcl] & 0x3F;
                uint16_t head = regs.byteregs[regdh];
                uint16_t count = regs.byteregs[regal];
                read_sectors(drive, segregs[reges], getreg16(regbx),
                            cyl, sect, head, count);
            } else {
                cf = 1;
                regs.byteregs[regah] = 0x01; /* Invalid function / not ready */
            }
            break;

        case 0x03: /* Write sectors */
            if (idx >= 0 && s_geo[idx].inserted) {
                uint16_t cyl = regs.byteregs[regch] +
                               ((uint16_t)(regs.byteregs[regcl] >> 6) << 8);
                uint16_t sect = regs.byteregs[regcl] & 0x3F;
                uint16_t head = regs.byteregs[regdh];
                uint16_t count = regs.byteregs[regal];
                write_sectors(drive, segregs[reges], getreg16(regbx),
                             cyl, sect, head, count);
            } else {
                cf = 1;
                regs.byteregs[regah] = 0x01;
            }
            break;

        case 0x04: /* Verify sectors (no-op, success) */
        case 0x05: /* Format track (no-op, success) */
            cf = 0;
            regs.byteregs[regah] = 0;
            break;

        case 0x08: /* Get drive parameters */
            if (idx >= 0 && s_geo[idx].inserted) {
                cf = 0;
                regs.byteregs[regah] = 0;
                regs.byteregs[regch] = (uint8_t)((s_geo[idx].cyls - 1) & 0xFF);
                regs.byteregs[regcl] = (s_geo[idx].sects & 0x3F) |
                                       (uint8_t)(((s_geo[idx].cyls - 1) >> 8) << 6);
                regs.byteregs[regdh] = (uint8_t)(s_geo[idx].heads - 1);
                if (drive < 0x80) {
                    regs.byteregs[regbl] = 4; /* drive type: 1.44MB */
                    regs.byteregs[regdl] = 2; /* number of floppy drives */
                } else {
                    regs.byteregs[regdl] = s_hdcount;
                }
            } else {
                cf = 1;
                regs.byteregs[regah] = 0xAA; /* Drive not ready */
            }
            break;

        case 0x15: /* Get disk type */
            if (idx >= 0 && s_geo[idx].inserted) {
                cf = 0;
                if (drive < 0x80) {
                    regs.byteregs[regah] = 0x01; /* Floppy without change-line */
                } else {
                    regs.byteregs[regah] = 0x03; /* Hard disk */
                    /* Return total sectors in CX:DX */
                    uint32_t total = (uint32_t)s_geo[idx].cyls *
                                     s_geo[idx].heads * s_geo[idx].sects;
                    regs.wordregs[regcx] = (uint16_t)(total >> 16);
                    regs.wordregs[regdx] = (uint16_t)(total & 0xFFFF);
                }
            } else {
                cf = 1;
                regs.byteregs[regah] = 0x00; /* No disk */
            }
            break;

        default:
            cf = 1;
            regs.byteregs[regah] = 0x01; /* Invalid function */
            break;
    }

    /* Save last status */
    if (idx >= 0) {
        s_last_ah[idx] = regs.byteregs[regah];
        s_last_cf[idx] = cf;
    }

    /* Store HD status in BDA */
    if (drive >= 0x80) {
        g_ram[0x0474] = regs.byteregs[regah];
    }
}

/* ---- Read sectors (public, for bootstrap) ---- */

void disk_read_chs(uint8_t drive, uint16_t dstseg, uint16_t dstoff,
                   uint16_t cyl, uint16_t sect, uint16_t head,
                   uint16_t sectcount)
{
    read_sectors(drive, dstseg, dstoff, cyl, sect, head, sectcount);
}

/* ---- INT 19h bootstrap ---- */

void disk_bootstrap(void) {
    if (bootdrive < 255) {
        regs.byteregs[regdl] = bootdrive;
        disk_read_chs(bootdrive, 0x07C0, 0x0000, 0, 1, 0, 1);
        segregs[regcs] = 0x0000;
        ip = 0x7C00;
    }
}
