#pragma once

#include <stdint.h>
#include <stdbool.h>

#define GB_WIDTH 160
#define GB_HEIGHT 144
#define SCALE 2
#define SCREEN_WIDTH (GB_WIDTH * SCALE)
#define SCREEN_HEIGHT (GB_HEIGHT * SCALE)

typedef struct {
    uint16_t framebuffer[GB_WIDTH * GB_HEIGHT];
    uint16_t palette[3][4];       // DMG palettes (BG, OBJ0, OBJ1)
    uint16_t cgb_lut[64];         // cached RGB565 lookup for CGB palette indices
    int selected_palette;
    int frame_count;
    bool cgb_mode;                // true when ROM is a GBC game
    const uint16_t *cgb_palette;  // pointer to gb->cgb.fixPalette[0x40] (RGB555)
} GBCDisplay;

void gbc_display_init(GBCDisplay *ctx);
void gbc_display_update_cgb_lut(GBCDisplay *ctx);
void gbc_display_set_palette(GBCDisplay *ctx, int palette_idx);
void gbc_display_next_palette(GBCDisplay *ctx);
void gbc_display_prev_palette(GBCDisplay *ctx);
void gbc_display_draw_line(GBCDisplay *ctx, const uint8_t pixels[GB_WIDTH], uint8_t line, void *gb_ctx);
void gbc_display_render(GBCDisplay *ctx,
                        void (*draw_image_nn_fn)(int, int, const uint16_t *, int, int, int),
                        void (*flush_fn)(void),
                        void (*flush_rows_fn)(int, int));
