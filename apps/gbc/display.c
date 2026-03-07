#include "display.h"
#include <string.h>

static const uint16_t default_dmg_palette[4] = {
    0xFFFF,  // shade 0: white
    0xAD55,  // shade 1: light gray
    0x528A,  // shade 2: dark gray
    0x0000   // shade 3: black
};

// Convert peanut_gb's RGB555 (R/B swapped) to RGB565
static inline uint16_t rgb555_to_rgb565(uint16_t c) {
    uint16_t r = (c >> 10) & 0x1F;
    uint16_t g = (c >>  5) & 0x1F;
    uint16_t b = (c      ) & 0x1F;
    return (r << 11) | (g << 6) | b;
}

void gbc_display_init(GBCDisplay *ctx) {
    for (int i = 0; i < 3; i++)
        memcpy(ctx->palette[i], default_dmg_palette, sizeof(default_dmg_palette));
    ctx->selected_palette = -1;
    ctx->frame_count = 0;
    ctx->cgb_mode = false;
    ctx->cgb_palette = NULL;
}

void gbc_display_set_palette(GBCDisplay *ctx, int palette_idx) {
    if (palette_idx >= 0 && palette_idx < 13) {
        ctx->selected_palette = palette_idx;
    }
}

void gbc_display_next_palette(GBCDisplay *ctx) {
    ctx->selected_palette++;
    if (ctx->selected_palette >= 13) {
        ctx->selected_palette = -1;
    }
}

void gbc_display_prev_palette(GBCDisplay *ctx) {
    ctx->selected_palette--;
    if (ctx->selected_palette < -1) {
        ctx->selected_palette = 12;
    }
}

void gbc_display_draw_line(GBCDisplay *ctx, const uint8_t pixels[GB_WIDTH], uint8_t line, void *gb_ctx) {
    (void)gb_ctx;
    if (line >= GB_HEIGHT) {
        return;
    }

    uint16_t *row = &ctx->framebuffer[line * GB_WIDTH];

    if (ctx->cgb_mode && ctx->cgb_palette) {
        // CGB mode: pixel value is an index into fixPalette[0x40]
        // BG: ((palette & 0x07) << 2) + shade  = 0x00-0x1F
        // OBJ: ((palette & 0x07) << 2) + shade + 0x20 = 0x20-0x3F
        for (int x = 0; x < GB_WIDTH; x++) {
            uint8_t idx = pixels[x] & 0x3F;
            row[x] = rgb555_to_rgb565(ctx->cgb_palette[idx]);
        }
    } else {
        // DMG mode: shade in bits 0-1, palette layer in bits 4-5
        for (int x = 0; x < GB_WIDTH; x++) {
            uint8_t pixel = pixels[x];
            uint8_t palette_idx = (pixel >> 4) & 0x0F;
            uint8_t shade = pixel & 0x03;
            row[x] = ctx->palette[palette_idx >> 2][shade];
        }
    }

    ctx->frame_count++;
}

void gbc_display_render(GBCDisplay *ctx,
                        void (*draw_image_nn_fn)(int, int, const uint16_t *, int, int, int),
                        void (*flush_fn)(void),
                        void (*flush_rows_fn)(int, int)) {
    if (ctx->frame_count == 0) {
        return;
    }

    draw_image_nn_fn(0, 16, ctx->framebuffer, GB_WIDTH, GB_HEIGHT, SCALE);

    if (flush_rows_fn) {
        // Flush only the dirty region: FPS bar (row 0) through GBC image bottom
        // (row 16 + 144*SCALE - 1 = 303 at SCALE 2)
        flush_rows_fn(0, 16 + GB_HEIGHT * SCALE - 1);
    } else {
        flush_fn();
    }

    ctx->frame_count = 0;
}
