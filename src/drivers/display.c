#include "display.h"
#include "../hardware.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "pico/mutex.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

// ── Framebuffer ──────────────────────────────────────────────────────────────
// Placed in PSRAM via linker section on Pimoroni Pico Plus 2W.
// On standard Pico 2 this will go to SRAM (204KB — tight, but usable).

#ifdef PICO_RP2350
// PSRAM section attribute — only works with Pimoroni SDK / linker script
__attribute__((section(".psram")))
static uint16_t s_framebuffer[FB_WIDTH * FB_HEIGHT];
#else
static uint16_t s_framebuffer[FB_WIDTH * FB_HEIGHT];
#endif

// Mutex to protect framebuffer between Core 0 (draw) and Core 1 (flush)
// and between LCD and WiFi SPI bus access
static mutex_t s_spi_mutex;

// DMA channel for LCD transfers
static int s_dma_chan = -1;

// ── Built-in 6x8 font (ASCII 0x20–0x7E) ─────────────────────────────────────
// Minimal 6x8 pixel font data — each character is 6 bytes (columns), 8 rows.
// This is a standard "font6x8" pattern used widely in embedded projects.
// Replace with a nicer font by swapping this array and updating FONT_W/H.

#define FONT_W  6
#define FONT_H  8

// Minimal ASCII 6x8 font (chars 0x20 to 0x7E = 95 characters)
// Format: column bytes, LSB = top pixel
static const uint8_t s_font6x8[95][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14,0x00}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, // '$'
    {0x23,0x13,0x08,0x64,0x62,0x00}, // '%'
    {0x36,0x49,0x55,0x22,0x50,0x00}, // '&'
    {0x00,0x05,0x03,0x00,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00,0x00}, // ')'
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, // '*'
    {0x08,0x08,0x3E,0x08,0x08,0x00}, // '+'
    {0x00,0x50,0x30,0x00,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08,0x00}, // '-'
    {0x00,0x60,0x60,0x00,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02,0x00}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, // '0'
    {0x00,0x42,0x7F,0x40,0x00,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46,0x00}, // '2'
    {0x21,0x41,0x45,0x4B,0x31,0x00}, // '3'
    {0x18,0x14,0x12,0x7F,0x10,0x00}, // '4'
    {0x27,0x45,0x45,0x45,0x39,0x00}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, // '6'
    {0x01,0x71,0x09,0x05,0x03,0x00}, // '7'
    {0x36,0x49,0x49,0x49,0x36,0x00}, // '8'
    {0x06,0x49,0x49,0x29,0x1E,0x00}, // '9'
    {0x00,0x36,0x36,0x00,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00,0x00}, // ';'
    {0x00,0x08,0x14,0x22,0x41,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14,0x00}, // '='
    {0x41,0x22,0x14,0x08,0x00,0x00}, // '>'
    {0x02,0x01,0x51,0x09,0x06,0x00}, // '?'
    {0x32,0x49,0x79,0x41,0x3E,0x00}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36,0x00}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22,0x00}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41,0x00}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01,0x00}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A,0x00}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01,0x00}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41,0x00}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40,0x00}, // 'L'
    {0x7F,0x02,0x04,0x02,0x7F,0x00}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06,0x00}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46,0x00}, // 'R'
    {0x46,0x49,0x49,0x49,0x31,0x00}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01,0x00}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F,0x00}, // 'W'
    {0x63,0x14,0x08,0x14,0x63,0x00}, // 'X'
    {0x07,0x08,0x70,0x08,0x07,0x00}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43,0x00}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20,0x00}, // '\'
    {0x00,0x41,0x41,0x7F,0x00,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04,0x00}, // '^'
    {0x40,0x40,0x40,0x40,0x40,0x00}, // '_'
    {0x00,0x01,0x02,0x04,0x00,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78,0x00}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38,0x00}, // 'b'
    {0x38,0x44,0x44,0x44,0x20,0x00}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F,0x00}, // 'd'
    {0x38,0x54,0x54,0x54,0x18,0x00}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02,0x00}, // 'f'
    {0x08,0x14,0x54,0x54,0x3C,0x00}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78,0x00}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00,0x00}, // 'j'
    {0x7F,0x10,0x28,0x44,0x00,0x00}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78,0x00}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78,0x00}, // 'n'
    {0x38,0x44,0x44,0x44,0x38,0x00}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08,0x00}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C,0x00}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08,0x00}, // 'r'
    {0x48,0x54,0x54,0x54,0x20,0x00}, // 's'
    {0x04,0x3F,0x44,0x40,0x20,0x00}, // 't'
    {0x3C,0x40,0x40,0x20,0x7C,0x00}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, // 'w'
    {0x44,0x28,0x10,0x28,0x44,0x00}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C,0x00}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44,0x00}, // 'z'
    {0x00,0x08,0x36,0x41,0x00,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00,0x00}, // '}'
    {0x08,0x08,0x2A,0x1C,0x08,0x00}, // '~' (→ arrow, used as placeholder)
};

// ── ST7365P Command set ───────────────────────────────────────────────────────

#define ST7365P_NOP        0x00
#define ST7365P_SWRST      0x01
#define ST7365P_SLPOUT     0x11
#define ST7365P_NORON      0x13
#define ST7365P_INVON      0x21    // Inversion on (may be needed depending on panel)
#define ST7365P_DISPON     0x29
#define ST7365P_CASET      0x2A
#define ST7365P_RASET      0x2B
#define ST7365P_RAMWR      0x2C
#define ST7365P_MADCTL     0x36
#define ST7365P_COLMOD     0x3A

// MADCTL bits
#define MADCTL_MX  0x40   // Mirror X
#define MADCTL_MY  0x80   // Mirror Y
#define MADCTL_MV  0x20   // Row/column exchange (landscape)
#define MADCTL_BGR 0x08   // BGR order (vs RGB)

// ── Low-level SPI helpers ─────────────────────────────────────────────────────

static inline void lcd_cs_low(void)  { gpio_put(LCD_PIN_CS, 0); }
static inline void lcd_cs_high(void) { gpio_put(LCD_PIN_CS, 1); }
static inline void lcd_dc_cmd(void)  { gpio_put(LCD_PIN_DC, 0); }
static inline void lcd_dc_data(void) { gpio_put(LCD_PIN_DC, 1); }

static void lcd_write_cmd(uint8_t cmd) {
    lcd_cs_low();
    lcd_dc_cmd();
    spi_write_blocking(LCD_SPI_PORT, &cmd, 1);
    lcd_cs_high();
}

static void lcd_write_data(const uint8_t *data, size_t len) {
    lcd_cs_low();
    lcd_dc_data();
    spi_write_blocking(LCD_SPI_PORT, data, len);
    lcd_cs_high();
}

static void lcd_write_byte(uint8_t b) {
    lcd_write_data(&b, 1);
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    // Optimized: batch all commands with minimal CS toggling
    lcd_cs_low();
    
    // CASET (column address set)
    lcd_dc_cmd();
    uint8_t caset = ST7365P_CASET;
    spi_write_blocking(LCD_SPI_PORT, &caset, 1);
    lcd_dc_data();
    uint8_t col_data[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    spi_write_blocking(LCD_SPI_PORT, col_data, 4);

    // RASET (row address set)
    lcd_dc_cmd();
    uint8_t raset = ST7365P_RASET;
    spi_write_blocking(LCD_SPI_PORT, &raset, 1);
    lcd_dc_data();
    uint8_t row_data[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    spi_write_blocking(LCD_SPI_PORT, row_data, 4);

    // RAMWR (start writing pixel data)
    lcd_dc_cmd();
    uint8_t ramwr = ST7365P_RAMWR;
    spi_write_blocking(LCD_SPI_PORT, &ramwr, 1);
    
    lcd_cs_high();
}

// ── Init sequence ─────────────────────────────────────────────────────────────
// Ported directly from the working constellation-pico Rust project (st7789.rs).
// No ST7796S-style 0xF0 manufacturer unlock — the panel responds to the standard
// ST7789 init sequence. Backlight is controlled by the STM32 keyboard MCU.

void display_init(void) {
    mutex_init(&s_spi_mutex);

    // Configure SPI1 (ST7789 Mode 3: CPOL=1, CPHA=1)
    uint32_t actual_baud = spi_init(LCD_SPI_PORT, LCD_SPI_BAUD);
    printf("[LCD] SPI requested: %lu Hz, actual: %lu Hz\n", (unsigned long)LCD_SPI_BAUD, (unsigned long)actual_baud);
    spi_set_format(LCD_SPI_PORT, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(LCD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_SCK,  GPIO_FUNC_SPI);

    // Control pins as GPIO outputs
    gpio_init(LCD_PIN_CS);  gpio_set_dir(LCD_PIN_CS,  GPIO_OUT); lcd_cs_high();
    gpio_init(LCD_PIN_DC);  gpio_set_dir(LCD_PIN_DC,  GPIO_OUT); lcd_dc_cmd();
    gpio_init(LCD_PIN_RST); gpio_set_dir(LCD_PIN_RST, GPIO_OUT);

    // Hardware reset — pulse RST low for 10 ms, then high, wait 120 ms
    gpio_put(LCD_PIN_RST, 0); sleep_ms(10);
    gpio_put(LCD_PIN_RST, 1); sleep_ms(120);

    printf("[LCD] init start\n");

    // Software reset — wait 150 ms before further commands
    lcd_write_cmd(ST7365P_SWRST); sleep_ms(150);

    // Sleep out — display enters normal operation, wait 50 ms
    lcd_write_cmd(ST7365P_SLPOUT); sleep_ms(50);

    // Colour mode: 16-bit RGB565
    lcd_write_cmd(ST7365P_COLMOD); lcd_write_byte(0x55);

    // Memory access control: MX (flip X) + BGR colour order
    lcd_write_cmd(ST7365P_MADCTL); lcd_write_byte(MADCTL_MX | MADCTL_BGR);

    // Frame rate control (FRMCTR2 / 0xB2)
    { uint8_t d[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
      lcd_write_cmd(0xB2); lcd_write_data(d, 5); }

    // Power control 1 (PWCTR1 / 0xC0)
    { uint8_t d[] = {0xA4, 0xA1};
      lcd_write_cmd(0xC0); lcd_write_data(d, 2); }

    // Positive gamma correction (GMCTRP1 / 0xE0)
    { uint8_t d[] = {0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23};
      lcd_write_cmd(0xE0); lcd_write_data(d, 14); }

    // Negative gamma correction (GMCTRN1 / 0xE1)
    { uint8_t d[] = {0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23};
      lcd_write_cmd(0xE1); lcd_write_data(d, 14); }

    // Inversion on (required for correct colours on this panel)
    lcd_write_cmd(ST7365P_INVON); sleep_ms(10);

    // Normal display mode on
    lcd_write_cmd(ST7365P_NORON); sleep_ms(10);

    // Display on
    lcd_write_cmd(ST7365P_DISPON); sleep_ms(10);

    printf("[LCD] init done, flushing black fill...\n");

    // DMA channel for framebuffer flushes.
    // Must use DMA_SIZE_8 with an 8-bit SPI: a DMA_SIZE_16 write to the SPI
    // FIFO in 8-bit mode only transmits bits[7:0], dropping the upper byte of
    // every pixel. DMA_SIZE_8 reads the framebuffer byte-by-byte so the
    // little-endian memory layout of our byte-swapped RGB565 values sends
    // the high byte first over the wire, as the display expects.
    s_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(s_dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_dreq(&cfg, spi_get_dreq(LCD_SPI_PORT, true));
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    dma_channel_configure(s_dma_chan, &cfg,
        &spi_get_hw(LCD_SPI_PORT)->dr,
        s_framebuffer,
        FB_SIZE,       // FB_WIDTH * FB_HEIGHT * 2 bytes
        false
    );

    // Fill black to confirm flush path works
    display_clear(COLOR_BLACK);
    display_flush();
    printf("[LCD] flush done\n");
}

void display_deinit(void) {
    if (s_dma_chan >= 0) dma_channel_unclaim(s_dma_chan);
    spi_deinit(LCD_SPI_PORT);
}

// ── Drawing functions ─────────────────────────────────────────────────────────

void display_clear(uint16_t color) {
    // Big-endian swap: ST7365P wants bytes big-endian over SPI
    uint16_t be = (color >> 8) | (color << 8);
    
    // Fast path: use memset for black (0x0000) or white (0xFFFF)
    if (be == 0x0000 || be == 0xFFFF) {
        memset(s_framebuffer, be & 0xFF, FB_SIZE);
        return;
    }
    
    // Optimized: fill in 32-pixel chunks for better memory bandwidth
    uint32_t color32 = ((uint32_t)be << 16) | be;  // Two pixels
    uint32_t *fb32 = (uint32_t *)s_framebuffer;
    size_t count32 = (FB_WIDTH * FB_HEIGHT) / 2;   // Number of 32-bit words
    
    for (size_t i = 0; i < count32; i++) {
        fb32[i] = color32;
    }
}

void display_set_pixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) return;
    uint16_t be = (color >> 8) | (color << 8);
    s_framebuffer[y * FB_WIDTH + x] = be;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_WIDTH)  w = FB_WIDTH  - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    uint16_t be = (color >> 8) | (color << 8);
    
    // Optimize for full-width fills
    if (x == 0 && w == FB_WIDTH) {
        uint32_t color32 = ((uint32_t)be << 16) | be;
        uint32_t *fb32 = (uint32_t *)&s_framebuffer[y * FB_WIDTH];
        size_t count32 = (w * h) / 2;
        for (size_t i = 0; i < count32; i++) {
            fb32[i] = color32;
        }
        return;
    }
    
    // Standard per-row fill
    for (int row = y; row < y + h; row++) {
        uint16_t *p = &s_framebuffer[row * FB_WIDTH + x];
        for (int col = 0; col < w; col++) p[col] = be;
    }
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color) {
    display_fill_rect(x,         y,         w, 1, color);
    display_fill_rect(x,         y + h - 1, w, 1, color);
    display_fill_rect(x,         y,         1, h, color);
    display_fill_rect(x + w - 1, y,         1, h, color);
}

void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    // Bresenham's line algorithm
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (true) {
        display_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

int display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
    int start_x = x;
    uint16_t fg_be = (fg >> 8) | (fg << 8);
    uint16_t bg_be = (bg >> 8) | (bg << 8);

    while (*text) {
        char c = *text++;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *glyph = s_font6x8[c - 0x20];

        for (int col = 0; col < FONT_W; col++) {
            uint8_t coldata = glyph[col];
            for (int row = 0; row < FONT_H; row++) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < FB_WIDTH && py >= 0 && py < FB_HEIGHT) {
                    s_framebuffer[py * FB_WIDTH + px] =
                        (coldata & (1 << row)) ? fg_be : bg_be;
                }
            }
        }
        x += FONT_W;
    }
    return x - start_x;
}

int display_text_width(const char *text) {
    int len = 0;
    while (*text++) len++;
    return len * FONT_W;
}

void display_draw_image(int x, int y, int w, int h, const uint16_t *data) {
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= FB_HEIGHT) continue;
        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < 0 || px >= FB_WIDTH) continue;
            uint16_t c = data[row * w + col];
            s_framebuffer[py * FB_WIDTH + px] = (c >> 8) | (c << 8);
        }
    }
}

void display_flush(void) {
    mutex_enter_blocking(&s_spi_mutex);

    lcd_set_window(0, 0, FB_WIDTH - 1, FB_HEIGHT - 1);

    lcd_cs_low();
    lcd_dc_data();

    // DMA transfer: non-blocking, CPU-free framebuffer → SPI
    dma_channel_set_read_addr(s_dma_chan, s_framebuffer, false);
    dma_channel_set_trans_count(s_dma_chan, FB_SIZE, true);  // start transfer

    // Wait for DMA completion
    dma_channel_wait_for_finish_blocking(s_dma_chan);

    // Ensure SPI shift register is fully drained before deasserting CS
    while (spi_is_busy(LCD_SPI_PORT)) tight_loop_contents();

    lcd_cs_high();
    mutex_exit(&s_spi_mutex);
}

void display_set_brightness(uint8_t brightness) {
    // Backlight is controlled by the STM32 keyboard MCU (kbd_set_backlight).
    // This function is a no-op on PicoCalc v2.0.
    (void)brightness;
}

void display_darken(void) {
    // Halve each byte of the framebuffer independently using 32-bit words.
    // The mask 0x7F7F7F7F prevents carry from one byte's LSB into the next.
    // Minor cross-channel colour shift at channel boundaries is imperceptible
    // in a menu overlay context.
    uint32_t *p = (uint32_t *)s_framebuffer;
    size_t n = (FB_WIDTH * FB_HEIGHT * 2) / 4;
    for (size_t i = 0; i < n; i++)
        p[i] = (p[i] >> 1) & 0x7F7F7F7FU;
}

const uint16_t *display_get_framebuffer(void) { return s_framebuffer; }

// ── SPI bus lock (called by WiFi driver to pause LCD DMA) ────────────────────
// WiFi driver should call these around any CYW43 SPI operations.

void display_spi_lock(void)   { mutex_enter_blocking(&s_spi_mutex); }
void display_spi_unlock(void) { mutex_exit(&s_spi_mutex); }
