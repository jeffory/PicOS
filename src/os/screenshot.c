#include "screenshot.h"
#include "../drivers/display.h"
#include "../drivers/sdcard.h"

#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

// BMP file layout constants
#define BMP_WIDTH      320
#define BMP_HEIGHT     320
#define BMP_ROW_BYTES  (BMP_WIDTH * 3)           // 960 — already 4-byte aligned
#define BMP_PIXEL_DATA 54                        // byte offset of pixel data
#define BMP_FILE_SIZE  (BMP_PIXEL_DATA + BMP_WIDTH * BMP_HEIGHT * 3)

// Write a little-endian 16-bit value into a byte buffer at offset.
static void put_u16le(uint8_t *buf, int off, uint16_t val) {
    buf[off]     = (uint8_t)(val & 0xFF);
    buf[off + 1] = (uint8_t)(val >> 8);
}

// Write a little-endian 32-bit value into a byte buffer at offset.
static void put_u32le(uint8_t *buf, int off, uint32_t val) {
    buf[off]     = (uint8_t)(val & 0xFF);
    buf[off + 1] = (uint8_t)((val >>  8) & 0xFF);
    buf[off + 2] = (uint8_t)((val >> 16) & 0xFF);
    buf[off + 3] = (uint8_t)((val >> 24) & 0xFF);
}

void screenshot_save(void) {
    // ── Find next available filename ─────────────────────────────────────────
    char path[32];
    int slot = -1;
    for (int n = 1; n <= 999; n++) {
        snprintf(path, sizeof(path), "/screenshots/shot%03d.bmp", n);
        if (!sdcard_fexists(path)) {
            slot = n;
            break;
        }
    }
    if (slot < 0) return;   // all 999 slots used — give up silently

    // ── Ensure directory exists ───────────────────────────────────────────────
    sdcard_mkdir("/screenshots");

    // ── Open file ─────────────────────────────────────────────────────────────
    sdfile_t f = sdcard_fopen(path, "w");
    if (!f) return;

    // ── Build 54-byte BMP header ──────────────────────────────────────────────
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));

    // BITMAPFILEHEADER (14 bytes)
    hdr[0] = 'B'; hdr[1] = 'M';                  // signature
    put_u32le(hdr,  2, (uint32_t)BMP_FILE_SIZE);  // file size
    // reserved: 4 + 6 bytes already zero
    put_u32le(hdr, 10, (uint32_t)BMP_PIXEL_DATA); // pixel data offset

    // BITMAPINFOHEADER (40 bytes starting at offset 14)
    put_u32le(hdr, 14, 40);                        // header size
    put_u32le(hdr, 18, (uint32_t)BMP_WIDTH);       // width
    // Negative height = top-down image (avoids row-reversal loop)
    put_u32le(hdr, 22, (uint32_t)(-BMP_HEIGHT));   // height (signed)
    put_u16le(hdr, 26, 1);                         // color planes
    put_u16le(hdr, 28, 24);                        // bits per pixel
    // compression (BI_RGB=0), image size, resolution, colors: all zero-filled

    sdcard_fwrite(f, hdr, sizeof(hdr));

    // ── Write pixel data ──────────────────────────────────────────────────────
    // s_framebuffer stores RGB565 big-endian (byte-swapped for the SPI display).
    // We must un-swap to recover the original RGB565 value, then expand to 8-bit
    // and write as BGR (BMP convention).

    const uint16_t *fb = display_get_framebuffer();
    uint8_t row_buf[BMP_ROW_BYTES];

    for (int y = 0; y < BMP_HEIGHT; y++) {
        for (int x = 0; x < BMP_WIDTH; x++) {
            uint16_t raw = fb[y * BMP_WIDTH + x];
            // Un-byte-swap: display stores pixels big-endian
            uint16_t px = (uint16_t)((raw >> 8) | (raw << 8));
            // Extract RGB565 channels
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >>  5) & 0x3F;
            uint8_t b5 = (px      ) & 0x1F;
            // Expand to 8-bit
            uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
            uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
            uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
            // BMP pixel order: BGR
            int idx = x * 3;
            row_buf[idx    ] = b8;
            row_buf[idx + 1] = g8;
            row_buf[idx + 2] = r8;
        }
        sdcard_fwrite(f, row_buf, BMP_ROW_BYTES);
    }

    sdcard_fclose(f);

    // ── Visual feedback ───────────────────────────────────────────────────────
    char msg[40];
    snprintf(msg, sizeof(msg), "Screenshot: shot%03d.bmp", slot);
    display_fill_rect(0, 0, display_text_width(msg) + 4, 10, COLOR_BLACK);
    display_draw_text(2, 1, msg, COLOR_WHITE, COLOR_BLACK);
    display_flush();
    sleep_ms(400);
}
