#pragma once

#include <stdbool.h>
#include <stdint.h>

// =============================================================================
// ST7365P Display Driver
// 320x320 IPS LCD via PIO SPI on PicoCalc mainboard v2.0
//
// Double-buffered framebuffers (2× 200 KB) reside in internal SRAM.
// The Lua heap is in PSRAM (6 MB), freeing SRAM for the framebuffers.
// DMA flushes run asynchronously so the CPU can draw the next frame.
//
// LCD uses a dedicated PIO SPI master (pio0), not a hardware SPI peripheral.
// =============================================================================

// RGB565 colour helpers
#define RGB565(r, g, b)                                                        \
  ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))

// Common colours
#define COLOR_BLACK RGB565(0, 0, 0)
#define COLOR_WHITE RGB565(255, 255, 255)
#define COLOR_RED RGB565(255, 0, 0)
#define COLOR_GREEN RGB565(0, 255, 0)
#define COLOR_BLUE RGB565(0, 0, 255)
#define COLOR_YELLOW RGB565(255, 255, 0)
#define COLOR_CYAN RGB565(0, 255, 255)
#define COLOR_GRAY RGB565(128, 128, 128)
#define COLOR_DKGRAY RGB565(64, 64, 64)

// Framebuffer size: 320*320*2 bytes = 204800 bytes (~200KB)
// This is stored in PSRAM on Pimoroni Pico Plus 2W
#define FB_WIDTH 320
#define FB_HEIGHT 320
#define FB_SIZE (FB_WIDTH * FB_HEIGHT * 2)

// Public init/deinit
void display_init(void);
void display_deinit(void);
void display_apply_clock(void);

// Drawing — all operations go to the framebuffer, not directly to LCD
// Call display_flush() to push to screen
void display_clear(uint16_t color);
void display_set_pixel(int x, int y, uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void display_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                           uint16_t color);
void display_draw_circle(int cx, int cy, int r, uint16_t color);
void display_fill_circle(int cx, int cy, int r, uint16_t color);

// Text rendering using the active bitmap font (default: 6x8)
// Returns pixel width of the rendered text
int display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
int display_text_width(const char *text);

// Font selection: 0 = 6x8 (default), 1 = 8x12, 2 = scientifica 6x12, 3 = scientifica-bold 6x12
void display_set_font(int font_id);
int display_get_font(void);
int display_get_font_width(void);
int display_get_font_height(void);

// Blit raw RGB565 image data to the framebuffer at (x, y).
// Pixel values must be in host byte order (same as the RGB565() macro).
// Out-of-bounds pixels are clipped silently.
void display_draw_image(int x, int y, int w, int h, const uint16_t *data);

// Blit a sub-rectangle of an image to the framebuffer at (x, y).
// sx, sy, sw, sh define the source rectangle within the w x h image.
// flip_x and flip_y mirror the drawing horizontally and vertically.
// transparent_color: 0 = use global setting, otherwise RGB565 color to treat as transparent.
void display_draw_image_partial(int x, int y, int img_w, int img_h,
                                const uint16_t *data, int sx, int sy, int sw,
                                int sh, bool flip_x, bool flip_y,
                                uint16_t transparent_color);

// Draw a scaled/rotated image to the framebuffer at (x, y).
// transparent_color: 0 = use global setting, otherwise RGB565 color to treat as transparent.
void display_draw_image_scaled(int x, int y, int img_w, int img_h,
                               const uint16_t *data, float scale, float angle,
                               uint16_t transparent_color);

// Draw a scaled image using nearest-neighbor (crisp pixel art).
// dst_w and dst_h specify the output size in pixels.
// transparent_color: 0 = use global setting, otherwise RGB565 color to treat as transparent.
void display_draw_image_scaled_nn(int x, int y, const uint16_t *data,
                                  int src_w, int src_h, int dst_w, int dst_h,
                                  uint16_t transparent_color);

// Draw an integer-scaled image using nearest-neighbor, optimised for speed.
// No transparency, no bounds check per pixel (pre-clamped).
// Ideal for emulator framebuffer blits (e.g. 160x144 @ 2x).
// Input data must be in host byte order (same as RGB565() macro).
void display_draw_image_nn(int x, int y, const uint16_t *data,
                           int src_w, int src_h, int scale);

// Blit pre-swapped (big-endian) RGB565 pixels directly to the framebuffer.
// No byte-swap is performed — data is memcpy'd row by row.
// Ideal for JPEGDEC with setPixelType(RGB565_BIG_ENDIAN).
void display_blit_be(int x, int y, const uint16_t *data, int w, int h);

// Transparent color key support (0 = disabled)
void display_set_transparent_color(uint16_t color);
uint16_t display_get_transparent_color(void);

// Push framebuffer to LCD.
// Default: non-blocking — DMA starts and returns immediately (CPU/DMA overlap).
// When g_display_flush_blocking is true, blocks until DMA completes.
// Safe for all apps: DMA reads SRAM framebuffers (AHB), CPU fetches from
// PSRAM (QMI/XIP) — separate buses.  Double buffering prevents conflicts.
void display_flush(void);
extern bool g_display_flush_blocking;

// Flush rows y0..y1 (inclusive) with a buffer swap, like display_flush() but
// only transfers the specified row range.  Useful for video playback where the
// content occupies a sub-region of the 320×320 screen.
void display_flush_region(int y0, int y1);

// Flush only rows y0..y1 (inclusive, full width) from the back buffer to the
// LCD. Does NOT swap buffers — call display_flush() for that.  Useful for
// partial screen updates (status bars, emulator viewports, etc.).
void display_flush_rows(int y0, int y1);

// Block until any in-flight DMA flush completes.
// Does NOT swap buffers or start a new transfer.
void display_wait_for_flush(void);

// Brightness via backlight PWM (0-255)
void display_set_brightness(uint8_t brightness);

// Halve the luminance of every pixel in the framebuffer in-place.
// Used by the system menu to create a translucent darkened overlay effect.
// Call before drawing the menu panel, then call display_flush().
void display_darken(void);

// Returns a read-only pointer to the raw framebuffer (320×320 RGB565,
// big-endian). Pixels are byte-swapped relative to the RGB565() macro — un-swap
// before use.
const uint16_t *display_get_framebuffer(void);
const uint16_t *display_get_front_buffer(void);

// Get writable pointer to the current back buffer for direct pixel writes.
// Pixels must be in big-endian RGB565 (byte-swapped from host order).
uint16_t *display_get_back_buffer(void);

// Wait for any ongoing DMA transfer to complete and return a pointer to the
// buffer that is currently visible on screen. This ensures screenshots capture
// the actual displayed frame, not a buffer still being transferred.
const uint16_t *display_get_screen_buffer(void);

// Hardware vertical scroll (ST7365P VSCRDEF + VSCRSADD).
// top_fixed + scroll_height + bottom_fixed must equal 320.
void display_set_scroll_area(int top_fixed, int scroll_height, int bottom_fixed);
void display_set_scroll_offset(int offset);

// =============================================================================
// Framebuffer Effects (post-processing shaders)
//
// All effects operate on the back buffer in-place. Draw first, apply effects,
// then call display_flush(). Effects wait for any in-flight DMA before
// modifying the framebuffer.
//
// Uses RP2350 SIO hardware interpolators (BLEND mode) for channel-level
// operations where beneficial.
// =============================================================================

typedef enum {
    EFFECT_INVERT,      // Bitwise invert all pixels
    EFFECT_DARKEN,      // Darken by factor (0-255, 128 = half brightness)
    EFFECT_BRIGHTEN,    // Brighten by factor (0-255, 128 = moderate)
    EFFECT_TINT,        // Blend framebuffer toward a tint color
    EFFECT_FADE,        // Fade toward a target color (same as tint, alias)
    EFFECT_GRAYSCALE,   // Desaturate using ITU-R BT.601 weights
    EFFECT_BLEND,       // Alpha-blend an external image onto the framebuffer
    EFFECT_PALETTE,     // Remap colors through a 256-entry LUT
    EFFECT_DITHER,      // Ordered Bayer 4x4 dithering
    EFFECT_SCANLINE,    // CRT-style scanline darkening
    EFFECT_POSTERIZE,   // Reduce color depth per channel
    EFFECT_COUNT
} display_effect_t;

// Individual effect functions (can be called directly from C)
void display_effect_invert(void);
void display_effect_darken(uint8_t factor);    // 0=black, 255=no change
void display_effect_brighten(uint8_t factor);  // 0=no change, 255=white
void display_effect_tint(uint8_t r, uint8_t g, uint8_t b, uint8_t strength);
void display_effect_grayscale(void);
void display_effect_blend(const uint16_t *src, int w, int h, uint8_t alpha);
void display_effect_palette(const uint16_t *lut, int lut_size);
void display_effect_dither(uint8_t levels);    // quantization levels per channel
void display_effect_scanline(uint8_t intensity); // 0=none, 255=black lines
void display_effect_posterize(uint8_t levels); // 2-32 levels per channel
