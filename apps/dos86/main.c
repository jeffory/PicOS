#include "app_abi.h"
#include "os.h"
#include <string.h>
#include <stdio.h>

// Emulated PC memory (allocated from QMI PSRAM at init)
static uint8_t *s_ram;       // 1 MB conventional + UMA
static uint8_t *s_vram;      // 256 KB video RAM
static uint8_t *s_portram;   // 64 KB I/O port state

#define RAM_SIZE   0x100000   // 1 MB
#define VRAM_SIZE  0x40000    // 256 KB
#define PORT_SIZE  0x10000    // 64 KB

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

    // Allocate emulated PC memory from QMI PSRAM
    s_ram = (uint8_t *)api->psram->qmiAlloc(RAM_SIZE);
    if (!s_ram) {
        api->sys->log("DOS86: Failed to allocate RAM");
        return;
    }
    memset(s_ram, 0, RAM_SIZE);

    s_vram = (uint8_t *)api->psram->qmiAlloc(VRAM_SIZE);
    if (!s_vram) {
        api->sys->log("DOS86: Failed to allocate VRAM");
        api->psram->qmiFree(s_ram);
        return;
    }
    memset(s_vram, 0, VRAM_SIZE);

    s_portram = (uint8_t *)api->psram->qmiAlloc(PORT_SIZE);
    if (!s_portram) {
        api->sys->log("DOS86: Failed to allocate port RAM");
        api->psram->qmiFree(s_vram);
        api->psram->qmiFree(s_ram);
        return;
    }
    memset(s_portram, 0, PORT_SIZE);

    api->sys->log("DOS86: PSRAM allocated (1.3 MB)");

    // Show placeholder screen
    api->display->clear(0x0000);
    api->display->drawText(100, 150, "DOS/86 v0.1", 0xFFFF, 0x0000);
    api->display->drawText(80, 170, "Press any key...", 0x7BEF, 0x0000);
    api->display->flush();

    while (!api->sys->shouldExit()) {
        api->sys->poll();
        if (api->input->getButtonsPressed() || api->input->getChar())
            break;
    }

    // Cleanup
    api->psram->qmiFree(s_portram);
    api->psram->qmiFree(s_vram);
    api->psram->qmiFree(s_ram);
    api->sys->log("DOS86: Exiting.");
}
