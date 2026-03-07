#include "input.h"
#include "os.h"

void gbc_input_init(GBCInput *ctx) {
    ctx->buttons = 0;
    ctx->prev_buttons = 0;
}

void gbc_input_update(GBCInput *ctx, uint32_t (*get_buttons_fn)(void)) {
    ctx->prev_buttons = ctx->buttons;
    ctx->buttons = get_buttons_fn();
}

void gbc_input_get_joypad(GBCInput *ctx, uint8_t *up, uint8_t *down, uint8_t *left, uint8_t *right, uint8_t *a, uint8_t *b, uint8_t *select, uint8_t *start) {
    *up     = (ctx->buttons & BTN_UP)    ? 0 : 1;
    *down  = (ctx->buttons & BTN_DOWN)  ? 0 : 1;
    *left  = (ctx->buttons & BTN_LEFT)  ? 0 : 1;
    *right = (ctx->buttons & BTN_RIGHT) ? 0 : 1;
    *a     = (ctx->buttons & BTN_F4)    ? 0 : 1;
    *b     = (ctx->buttons & BTN_F5)    ? 0 : 1;
    *select = (ctx->buttons & BTN_F1)   ? 0 : 1;
    *start = (ctx->buttons & BTN_F2)    ? 0 : 1;
}
