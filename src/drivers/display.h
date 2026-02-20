#pragma once

#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// ST7365P Display Driver
// 320x320 IPS LCD via SPI1 on PicoCalc mainboard v2.0
//
// The framebuffer lives in PSRAM (Pimoroni Pico Plus 2W provides 8MB).
// Core 1 handles periodic flush from framebuffer → LCD over SPI.
// Core 0 runs the OS + Lua. Both cores share the framebuffer via a mutex.
//
// SPI1 is also shared with WiFi (CYW43). The driver implements a lock that
// pauses LCD DMA transfers during WiFi operations and vice versa.
// =============================================================================

// RGB565 colour helpers
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))

// Common colours
#define COLOR_BLACK   RGB565(0,   0,   0  )
#define COLOR_WHITE   RGB565(255, 255, 255)
#define COLOR_RED     RGB565(255, 0,   0  )
#define COLOR_GREEN   RGB565(0,   255, 0  )
#define COLOR_BLUE    RGB565(0,   0,   255)
#define COLOR_YELLOW  RGB565(255, 255, 0  )
#define COLOR_CYAN    RGB565(0,   255, 255)
#define COLOR_GRAY    RGB565(128, 128, 128)
#define COLOR_DKGRAY  RGB565(64,  64,  64 )

// Framebuffer size: 320*320*2 bytes = 204800 bytes (~200KB)
// This is stored in PSRAM on Pimoroni Pico Plus 2W
#define FB_WIDTH   320
#define FB_HEIGHT  320
#define FB_SIZE    (FB_WIDTH * FB_HEIGHT * 2)

// Public init/deinit
void display_init(void);
void display_deinit(void);

// Drawing — all operations go to the framebuffer, not directly to LCD
// Call display_flush() to push to screen
void display_clear(uint16_t color);
void display_set_pixel(int x, int y, uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_rect(int x, int y, int w, int h, uint16_t color);
void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color);

// Text rendering using the built-in 6x8 bitmap font
// Returns pixel width of the rendered text
int  display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
int  display_text_width(const char *text);

// Blit raw RGB565 image data to the framebuffer at (x, y).
// Pixel values must be in host byte order (same as the RGB565() macro).
// Out-of-bounds pixels are clipped silently.
void display_draw_image(int x, int y, int w, int h, const uint16_t *data);

// Push framebuffer to LCD. Blocks until transfer complete.
// The WiFi SPI lock is acquired/released around this call.
void display_flush(void);

// Brightness via backlight PWM (0-255)
void display_set_brightness(uint8_t brightness);

// Halve the luminance of every pixel in the framebuffer in-place.
// Used by the system menu to create a translucent darkened overlay effect.
// Call before drawing the menu panel, then call display_flush().
void display_darken(void);

// Returns a read-only pointer to the raw framebuffer (320×320 RGB565, big-endian).
// Pixels are byte-swapped relative to the RGB565() macro — un-swap before use.
const uint16_t *display_get_framebuffer(void);

// SPI bus arbitration — the WiFi (CYW43) and LCD share SPI1.
// The WiFi driver must hold this lock for the duration of any CYW43 SPI
// operation to prevent bus conflicts with LCD DMA transfers.
void display_spi_lock(void);
void display_spi_unlock(void);
