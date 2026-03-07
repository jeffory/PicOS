#include "app_abi.h"
#include "os.h"
#include "display.h"

void picos_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    (void)app_dir;
    (void)app_id;
    (void)app_name;
    
    const picocalc_display_t *d = api->display;
    const picocalc_sys_t *sys = api->sys;
    const picocalc_input_t *in = api->input;
    
    d->clear(0x0000);
    d->drawText(80, 150, "With display.h", 0xFFFF, 0x0000);
    d->flush();
    
    sys->log("TEST: entering while loop");
    int iter = 0;
    while (1) {
        sys->poll();
        uint32_t btns = in->getButtonsPressed();
        if (btns & BTN_ESC) {
            break;
        }
        iter++;
        if (iter % 500 == 0) sys->log("TEST: iter %d still running", iter);
    }
    sys->log("TEST: exited loop cleanly at iter %d", iter);
}
