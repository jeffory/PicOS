#include "app_abi.h"
#include "os.h"
#include <string.h>
#include <stdio.h>
#include "fake86/config.h"
#include "fake86/cpu.h"
#include "fake86/i8259.h"
#include "fake86/i8253.h"
#include "fake86/ports.h"
#include "fake86/disk.h"
#include "fake86/video.h"
#include "fake86/render.h"
#include "backend.h"
#include "speaker.h"
#include "roms/pcxtbios.h"
#include "roms/videorom.h"

/* Global memory pointers (referenced by fake86 via extern in config.h) */
uint8_t *g_ram;       /* 1 MB conventional + UMA */
uint8_t *g_vram;      /* 256 KB video RAM */
uint8_t *g_portram;   /* 64 KB I/O port state */

static const PicoCalcAPI *s_api;

/* ---- Colors (RGB565) ---- */
#define COL_BG      0x0000  /* Black */
#define COL_FG      0xFFFF  /* White */
#define COL_TITLE   0x07E0  /* Green */
#define COL_SEL_BG  0x001F  /* Blue */
#define COL_SEL_FG  0xFFFF  /* White */
#define COL_DIM     0x7BEF  /* Gray */
#define COL_ERR     0xF800  /* Red */

/* ---- Disk image selector ---- */

#define DISK_DIR "/data/dos86/disks"
#define MAX_IMAGES 32
#define MAX_NAME_LEN 64

typedef struct {
    char name[MAX_NAME_LEN];
    uint32_t size;
} disk_entry_t;

static disk_entry_t s_images[MAX_IMAGES];
static int s_image_count;

static void list_callback(const char *name, bool is_dir, uint32_t size, void *user) {
    (void)user;
    if (is_dir) return;

    /* Only show .img files */
    int len = (int)strlen(name);
    if (len < 5) return;
    const char *ext = name + len - 4;
    if (ext[0] != '.' ||
        (ext[1] != 'i' && ext[1] != 'I') ||
        (ext[2] != 'm' && ext[2] != 'M') ||
        (ext[3] != 'g' && ext[3] != 'G'))
        return;

    if (s_image_count >= MAX_IMAGES) return;

    /* Copy name, truncating if needed */
    int copy_len = len < MAX_NAME_LEN - 1 ? len : MAX_NAME_LEN - 1;
    memcpy(s_images[s_image_count].name, name, copy_len);
    s_images[s_image_count].name[copy_len] = '\0';
    s_images[s_image_count].size = size;
    s_image_count++;
}

static const char *size_label(uint32_t size) {
    if (size == 1474560) return "1.44M";
    if (size == 1228800) return "1.2M";
    if (size == 737280)  return "720K";
    if (size == 368640)  return "360K";
    if (size == 163840)  return "160K";
    if (size >= 1048576) {
        static char buf[16];
        snprintf(buf, sizeof(buf), "%uM", (unsigned)(size / 1048576));
        return buf;
    }
    if (size >= 1024) {
        static char buf[16];
        snprintf(buf, sizeof(buf), "%uK", (unsigned)(size / 1024));
        return buf;
    }
    return "?";
}

/* Show disk image selector. Returns index of selected image, or -1 on cancel. */
static int show_disk_selector(const PicoCalcAPI *api) {
    s_image_count = 0;

    /* Ensure disk directory exists */
    api->fs->mkdir("/data");
    api->fs->mkdir("/data/dos86");
    api->fs->mkdir(DISK_DIR);

    /* Scan for .img files */
    api->fs->listDir(DISK_DIR, list_callback, NULL);

    if (s_image_count == 0) {
        api->display->clear(COL_BG);
        api->display->drawText(20, 100, "DOS/86 - No disk images found", COL_ERR, COL_BG);
        api->display->drawText(20, 120, "Place .img files in:", COL_FG, COL_BG);
        api->display->drawText(20, 140, DISK_DIR, COL_TITLE, COL_BG);
        api->display->drawText(20, 180, "Press any key to exit...", COL_DIM, COL_BG);
        api->display->flush();

        while (!api->sys->shouldExit()) {
            api->sys->poll();
            if (api->input->getButtonsPressed() || api->input->getChar())
                return -1;
        }
        return -1;
    }

    int sel = 0;
    int scroll = 0;
    const int ITEMS_PER_PAGE = 12;
    const int Y_START = 48;
    const int LINE_H = 20;

    for (;;) {
        if (api->sys->shouldExit()) return -1;
        api->sys->poll();

        uint32_t btns = api->input->getButtonsPressed();
        if (btns & BTN_UP) {
            if (sel > 0) sel--;
            if (sel < scroll) scroll = sel;
        }
        if (btns & BTN_DOWN) {
            if (sel < s_image_count - 1) sel++;
            if (sel >= scroll + ITEMS_PER_PAGE) scroll = sel - ITEMS_PER_PAGE + 1;
        }
        if (btns & BTN_ENTER) return sel;
        if (btns & BTN_ESC)   return -1;

        /* Draw */
        api->display->clear(COL_BG);
        api->display->drawText(20, 10, "DOS/86 - Select Boot Disk", COL_TITLE, COL_BG);

        char hdr[48];
        snprintf(hdr, sizeof(hdr), "%d image%s found in " DISK_DIR,
                 s_image_count, s_image_count == 1 ? "" : "s");
        api->display->drawText(20, 28, hdr, COL_DIM, COL_BG);

        for (int i = 0; i < ITEMS_PER_PAGE && (scroll + i) < s_image_count; i++) {
            int idx = scroll + i;
            int y = Y_START + i * LINE_H;
            uint16_t fg = (idx == sel) ? COL_SEL_FG : COL_FG;
            uint16_t bg = (idx == sel) ? COL_SEL_BG : COL_BG;

            if (idx == sel) {
                api->display->fillRect(16, y - 2, 288, LINE_H, COL_SEL_BG);
            }

            char line[64];
            snprintf(line, sizeof(line), " %-40s %s",
                     s_images[idx].name, size_label(s_images[idx].size));
            api->display->drawText(18, y, line, fg, bg);
        }

        /* Scroll indicators */
        if (scroll > 0)
            api->display->drawText(300, Y_START, "^", COL_DIM, COL_BG);
        if (scroll + ITEMS_PER_PAGE < s_image_count)
            api->display->drawText(300, Y_START + (ITEMS_PER_PAGE - 1) * LINE_H, "v", COL_DIM, COL_BG);

        api->display->drawText(20, 300, "[Enter] Boot  [Esc] Cancel", COL_DIM, COL_BG);
        api->display->flush();
    }
}

/* ---- Entry point ---- */

void picos_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    s_api = api;
    (void)app_id;
    (void)app_name;
    (void)app_dir;
    api->sys->log("DOS86: Starting...");

    /* Allocate emulated PC memory from QMI PSRAM */
    g_ram = (uint8_t *)api->psram->qmiAlloc(RAM_SIZE);
    if (!g_ram) {
        api->sys->log("DOS86: Failed to allocate RAM");
        return;
    }
    memset(g_ram, 0, RAM_SIZE);

    g_vram = (uint8_t *)api->psram->qmiAlloc(VRAM_SIZE);
    if (!g_vram) {
        api->sys->log("DOS86: Failed to allocate VRAM");
        api->psram->qmiFree(g_ram);
        return;
    }
    memset(g_vram, 0, VRAM_SIZE);

    g_portram = (uint8_t *)api->psram->qmiAlloc(PORT_SIZE);
    if (!g_portram) {
        api->sys->log("DOS86: Failed to allocate port RAM");
        api->psram->qmiFree(g_vram);
        api->psram->qmiFree(g_ram);
        return;
    }
    memset(g_portram, 0, PORT_SIZE);

    api->sys->log("DOS86: PSRAM allocated (1.3 MB)");

    /* Initialize backend and port dispatcher */
    backend_init(api, app_dir);
    ports_reset();

    /* Load BIOS ROM into F000:0000 (0xF0000-0xFFFFF).
       The pcxtbios.bin is 8KB — load it at the end of the 64KB ROM segment
       so the reset vector at F000:FFF0 is correct. */
    memset(&g_ram[0xF0000], 0xF4, 0x10000);  /* Fill with HLT first */
    {
        uint32_t bios_offset = 0x10000 - BIOS_ROM_SIZE;  /* 8KB ROM at end */
        if (BIOS_ROM_SIZE <= 0x10000) {
            memcpy(&g_ram[0xF0000 + bios_offset], bios_rom, BIOS_ROM_SIZE);
        }
    }
    api->sys->log("DOS86: BIOS ROM loaded");

    /* Load video ROM into C000:0000 (0xC0000-0xC7FFF) */
    {
        uint32_t vid_size = VIDEO_ROM_SIZE < 0x8000 ? VIDEO_ROM_SIZE : 0x8000;
        memcpy(&g_ram[0xC0000], video_rom, vid_size);
    }
    api->sys->log("DOS86: Video ROM loaded");

    /* Show disk selector UI */
    int sel = show_disk_selector(api);
    if (sel < 0) {
        /* User cancelled or no images */
        backend_shutdown();
        api->psram->qmiFree(g_portram);
        api->psram->qmiFree(g_vram);
        api->psram->qmiFree(g_ram);
        api->sys->log("DOS86: Cancelled.");
        return;
    }

    /* Mount selected image as drive A: (0x00) */
    {
        char path[128];
        snprintf(path, sizeof(path), "%s/%s", DISK_DIR, s_images[sel].name);

        if (!disk_mount_with_size(0x00, path, s_images[sel].size)) {
            api->display->clear(COL_BG);
            api->display->drawText(20, 140, "Failed to mount disk image!", COL_ERR, COL_BG);
            api->display->flush();
            while (!api->sys->shouldExit()) {
                api->sys->poll();
                if (api->input->getButtonsPressed()) break;
            }
            backend_shutdown();
            api->psram->qmiFree(g_portram);
            api->psram->qmiFree(g_vram);
            api->psram->qmiFree(g_ram);
            return;
        }
    }

    {
        char buf[80];
        snprintf(buf, sizeof(buf), "DOS86: Mounted '%s' as A:", s_images[sel].name);
        api->sys->log(buf);

        const disk_geo_t *geo = disk_get_geo(0x00);
        if (geo) {
            snprintf(buf, sizeof(buf), "DOS86: Geometry: C=%u H=%u S=%u (%s)",
                     geo->cyls, geo->heads, geo->sects, size_label(geo->size_bytes));
            api->sys->log(buf);
        }
    }

    /* Set boot drive to A: */
    bootdrive = 0x00;

    /* Reset CPU — CS:IP → F000:FFF0 (reset vector) */
    cpu_reset();
    api->sys->log("DOS86: CPU reset, entering emulation...");

    /* Initialize peripherals */
    i8259_reset();
    i8253_reset();
    speaker_init();
    video_reset();

    /* Allocate 320x200 RGB565 render buffer from PSRAM (128 KB) */
    uint16_t *render_buf = (uint16_t *)api->psram->qmiAlloc(320 * 200 * sizeof(uint16_t));
    if (!render_buf) {
        api->sys->log("DOS86: Failed to allocate render buffer");
        disk_unmount(0x00);
        backend_shutdown();
        api->psram->qmiFree(g_portram);
        api->psram->qmiFree(g_vram);
        api->psram->qmiFree(g_ram);
        return;
    }
    memset(render_buf, 0, 320 * 200 * sizeof(uint16_t));
    api->sys->log("DOS86: Render buffer allocated");

    /* Clear display for emulation */
    api->display->clear(COL_BG);
    api->display->flush();

    /* Main emulation loop */
    while (!api->sys->shouldExit()) {
        api->sys->poll();
        backend_pump_keyboard();

        /* Execute ~100K instructions per frame (~60 fps target) */
        exec86(100000);

        /* Tick the PIT: 1.19318 MHz / 60 Hz = ~19886 ticks per frame */
        i8253_tick(19886);

        /* Render video output when dirty */
        if (video_is_dirty()) {
            render_frame(render_buf);
            backend_render_frame(render_buf, 320, 200);
            video_clear_dirty();
        }
    }

    /* Cleanup */
    disk_unmount(0x00);
    backend_shutdown();
    api->psram->qmiFree(render_buf);
    api->psram->qmiFree(g_portram);
    api->psram->qmiFree(g_vram);
    api->psram->qmiFree(g_ram);
    api->sys->log("DOS86: Exiting.");
}
