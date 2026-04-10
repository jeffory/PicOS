#include "app_abi.h"
#include "os.h"
#include <string.h>
#include <stdio.h>
#include "fake86/config.h"
#include "fake86/cpu.h"
#include "fake86/i8259.h"
#include "fake86/i8253.h"
#include "fake86/ports.h"
#include "backend.h"
#include "speaker.h"

/* Global memory pointers (referenced by fake86 via extern in config.h) */
uint8_t *g_ram;       /* 1 MB conventional + UMA */
uint8_t *g_vram;      /* 256 KB video RAM */
uint8_t *g_portram;   /* 64 KB I/O port state */

static const PicoCalcAPI *s_api;

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

    /* Fill ROM area (0xF0000-0xFFFFF) with HLT instructions so the CPU
       halts immediately after reset instead of executing random data. */
    memset(&g_ram[0xF0000], 0xF4, 0x10000);  /* 0xF4 = HLT */

    /* Reset the CPU — sets CS:IP to F000:FFF0 (the x86 reset vector) */
    cpu_reset();
    api->sys->log("DOS86: CPU reset, CS:IP = F000:FFF0");

    /* Initialize peripherals */
    i8259_reset();
    i8253_reset();
    speaker_init();
    api->sys->log("DOS86: PIC/PIT/Speaker initialized");

    /* Execute a small number of instructions to prove the CPU works.
       The reset vector points into our HLT-filled ROM, so exec86 will
       execute exactly 1 HLT and then stop (hltstate=1). */
    exec86(100);

    {
        char buf[80];
        snprintf(buf, sizeof(buf), "DOS86: exec86 done, totalexec=%u hlt=%u",
                 (unsigned)totalexec, (unsigned)hltstate);
        api->sys->log(buf);
    }

    /* Show status on screen */
    api->display->clear(0x0000);
    api->display->drawText(60, 140, "DOS/86 v0.2 - CPU OK", 0x07E0, 0x0000);
    {
        char buf[80];
        snprintf(buf, sizeof(buf), "Instructions: %u  HLT: %s",
                 (unsigned)totalexec, hltstate ? "yes" : "no");
        api->display->drawText(60, 160, buf, 0xFFFF, 0x0000);
        snprintf(buf, sizeof(buf), "CS:IP = %04X:%04X",
                 segregs[regcs], ip);
        api->display->drawText(60, 180, buf, 0xFFFF, 0x0000);
    }
    api->display->drawText(80, 210, "Press any key...", 0x7BEF, 0x0000);
    api->display->flush();

    while (!api->sys->shouldExit()) {
        api->sys->poll();
        if (api->input->getButtonsPressed() || api->input->getChar())
            break;
    }

    /* Cleanup */
    backend_shutdown();
    api->psram->qmiFree(g_portram);
    api->psram->qmiFree(g_vram);
    api->psram->qmiFree(g_ram);
    api->sys->log("DOS86: Exiting.");
}
