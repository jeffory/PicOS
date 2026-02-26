#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int w;
  int h;
  uint16_t *data; // RGB565 format buffer, to be freed by caller (umm_free)
} image_decode_result_t;

// Decodes a JPEG buffer into the result object
// Returns true on success, false on failure
bool decode_jpeg_buffer(const uint8_t *data, size_t len,
                        image_decode_result_t *result);

// Decodes a JPEG directly from a file path using FatFS streaming
bool decode_jpeg_file(const char *path, image_decode_result_t *result);

// Decodes a PNG buffer into the result object
// Returns true on success, false on failure
bool decode_png_buffer(const uint8_t *data, size_t len,
                       image_decode_result_t *result);

// Decodes a PNG directly from a file path using FatFS streaming
bool decode_png_file(const char *path, image_decode_result_t *result);

// Decodes the first frame of a GIF buffer into the result object
// Returns true on success, false on failure
bool decode_gif_buffer(const uint8_t *data, size_t len,
                       image_decode_result_t *result);

// Decodes a GIF directly from a file path using FatFS streaming
bool decode_gif_file(const char *path, image_decode_result_t *result);

// Draws a scaled/rotated image using tgx onto the destination framebuffer.
// Both buffers must be in RGB565 format.
void tgx_draw_image_scaled(uint16_t *dst_fb, int dst_w, int dst_h,
                           const uint16_t *src_data, int src_w, int src_h,
                           int dst_x, int dst_y, float scale, float angle);

// Draws a scaled/rotated image using tgx with color-key transparency.
// transparent_color: RGB565 color that will be treated as transparent (skipped).
void tgx_draw_image_scaled_masked(uint16_t *dst_fb, int dst_w, int dst_h,
                                  const uint16_t *src_data, int src_w, int src_h,
                                  int dst_x, int dst_y, float scale, float angle,
                                  uint16_t transparent_color);

#ifdef __cplusplus
}
#endif
