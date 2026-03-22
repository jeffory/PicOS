#pragma once
#include <stdint.h>
#include <stdbool.h>

// Concrete image struct. Fields accessible to OS internals; pcimage_t (void*)
// is the ABI-safe opaque handle for native apps via g_api.
typedef struct {
    int       w;
    int       h;
    uint16_t *data;              // RGB565 pixel buffer in PSRAM (umm_malloc)
    uint16_t  transparent_color; // 0 = disabled
} pc_image_t;

// Load image from file path (BMP, JPEG, PNG, GIF).
// Returns heap-allocated pc_image_t* (struct + data both in PSRAM via umm_malloc).
// Returns NULL on failure.
pc_image_t *image_load(const char *path);

// Allocate a blank zeroed image (struct + data in PSRAM).
// Returns NULL on OOM or invalid dimensions.
pc_image_t *image_new_blank(int width, int height);

// Free an image. Frees pixel data AND the struct itself. Safe to call with NULL.
void image_free(pc_image_t *img);

// Drawing operations. Use img->transparent_color for transparency keying (0 = none).
void image_draw(const pc_image_t *img, int x, int y);
void image_draw_region(const pc_image_t *img,
                       int sx, int sy, int sw, int sh,
                       int dx, int dy);
void image_draw_scaled(const pc_image_t *img, int x, int y, int dst_w, int dst_h);
