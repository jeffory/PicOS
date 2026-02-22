#include "display.h"
#include "../hardware.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "hardware/xip_cache.h"
#include "lcd_spi.pio.h"
#include "pico/stdlib.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../os/image_decoders.h"

// ── Framebuffer ──────────────────────────────────────────────────────────────
// Placed in internal SRAM smoothly now that the Lua heap has been relocated
// to PSRAM, freeing up 256KB of internal memory!
static uint16_t s_framebuffers[2][FB_WIDTH * FB_HEIGHT];

// Pointer to the current back buffer
static uint16_t *s_framebuffer = s_framebuffers[0];
static int s_back_buffer_idx = 0;
static bool s_dma_active = false;

// DMA channel for LCD transfers
static int s_dma_chan = -1;
static uint s_pio_sm = 0;

// ── Built-in 6x8 font (ASCII 0x20–0x7E) ─────────────────────────────────────
// Minimal 6x8 pixel font data — each character is 6 bytes (columns), 8 rows.
// This is a standard "font6x8" pattern used widely in embedded projects.
// Replace with a nicer font by swapping this array and updating FONT_W/H.

#define FONT_W 6
#define FONT_H 8

// Minimal ASCII 6x8 font (chars 0x20 to 0x7E = 95 characters)
// Format: column bytes, LSB = top pixel
static const uint8_t s_font6x8[95][6] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00, 0x00}, // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00, 0x00}, // '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14, 0x00}, // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12, 0x00}, // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62, 0x00}, // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50, 0x00}, // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00, 0x00}, // '''
    {0x00, 0x1C, 0x22, 0x41, 0x00, 0x00}, // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00, 0x00}, // ')'
    {0x08, 0x2A, 0x1C, 0x2A, 0x08, 0x00}, // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08, 0x00}, // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00, 0x00}, // ','
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x00}, // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00, 0x00}, // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02, 0x00}, // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00}, // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00, 0x00}, // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46, 0x00}, // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31, 0x00}, // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10, 0x00}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39, 0x00}, // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30, 0x00}, // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03, 0x00}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36, 0x00}, // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E, 0x00}, // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00, 0x00}, // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00, 0x00}, // ';'
    {0x00, 0x08, 0x14, 0x22, 0x41, 0x00}, // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14, 0x00}, // '='
    {0x41, 0x22, 0x14, 0x08, 0x00, 0x00}, // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06, 0x00}, // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E, 0x00}, // '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00}, // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36, 0x00}, // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22, 0x00}, // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C, 0x00}, // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41, 0x00}, // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01, 0x00}, // 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A, 0x00}, // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00}, // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00, 0x00}, // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01, 0x00}, // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41, 0x00}, // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40, 0x00}, // 'L'
    {0x7F, 0x02, 0x04, 0x02, 0x7F, 0x00}, // 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00}, // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00}, // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06, 0x00}, // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E, 0x00}, // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46, 0x00}, // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31, 0x00}, // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01, 0x00}, // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F, 0x00}, // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00}, // 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F, 0x00}, // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63, 0x00}, // 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07, 0x00}, // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43, 0x00}, // 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00, 0x00}, // '['
    {0x02, 0x04, 0x08, 0x10, 0x20, 0x00}, // '\'
    {0x00, 0x41, 0x41, 0x7F, 0x00, 0x00}, // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04, 0x00}, // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40, 0x00}, // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00, 0x00}, // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78, 0x00}, // 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38, 0x00}, // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20, 0x00}, // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F, 0x00}, // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18, 0x00}, // 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02, 0x00}, // 'f'
    {0x08, 0x14, 0x54, 0x54, 0x3C, 0x00}, // 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78, 0x00}, // 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00, 0x00}, // 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00, 0x00}, // 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00, 0x00}, // 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00, 0x00}, // 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78, 0x00}, // 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78, 0x00}, // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38, 0x00}, // 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08, 0x00}, // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C, 0x00}, // 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08, 0x00}, // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20, 0x00}, // 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20, 0x00}, // 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C, 0x00}, // 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C, 0x00}, // 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C, 0x00}, // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44, 0x00}, // 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C, 0x00}, // 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44, 0x00}, // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00, 0x00}, // '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00, 0x00}, // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00, 0x00}, // '}'
    {0x08, 0x08, 0x2A, 0x1C, 0x08, 0x00}, // '~' (→ arrow, used as placeholder)
};

// ── ST7365P Command set
// ───────────────────────────────────────────────────────

#define ST7365P_NOP 0x00
#define ST7365P_SWRST 0x01
#define ST7365P_SLPOUT 0x11
#define ST7365P_NORON 0x13
#define ST7365P_INVOFF 0x20
#define ST7365P_INVON 0x21 // Inversion on (may be needed depending on panel)
#define ST7365P_DISPON 0x29
#define ST7365P_CASET 0x2A
#define ST7365P_RASET 0x2B
#define ST7365P_RAMWR 0x2C
#define ST7365P_MADCTL 0x36
#define ST7365P_COLMOD 0x3A

// MADCTL bits
#define MADCTL_MX 0x40  // Mirror X
#define MADCTL_MY 0x80  // Mirror Y
#define MADCTL_MV 0x20  // Row/column exchange (landscape)
#define MADCTL_BGR 0x08 // BGR order (vs RGB)

// ── Low-level SPI helpers
// ─────────────────────────────────────────────────────

static inline void lcd_cs_low(void) { gpio_put(LCD_PIN_CS, 0); }
static inline void lcd_cs_high(void) { gpio_put(LCD_PIN_CS, 1); }
static inline void lcd_dc_cmd(void) { gpio_put(LCD_PIN_DC, 0); }
static inline void lcd_dc_data(void) { gpio_put(LCD_PIN_DC, 1); }

static inline void pio_spi_write8(uint8_t data) {
  pio_sm_put_blocking(LCD_PIO, s_pio_sm, (uint32_t)data << 24);
}

static inline void lcd_spi_wait_idle(void) {
  while (!pio_sm_is_tx_fifo_empty(LCD_PIO, s_pio_sm))
    tight_loop_contents();
  uint32_t stall_mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + s_pio_sm);
  LCD_PIO->fdebug = stall_mask;
  while (!(LCD_PIO->fdebug & stall_mask))
    tight_loop_contents();

  // The FIFO is physically empty and has stalled the state machine, but the
  // hardware Output Shift Register (OSR) is still holding the final bit chunk
  // and actively clocking it out! We must wait a few more cycles to guarantee
  // the trailing bits exit the screen controller wire before deasserting CS.
  busy_wait_us(1);
}

static void lcd_write_cmd(uint8_t cmd) {
  lcd_cs_low();
  lcd_dc_cmd();
  pio_spi_write8(cmd);
  lcd_spi_wait_idle();
  lcd_cs_high();
}

static void lcd_write_data(const uint8_t *data, size_t len) {
  lcd_cs_low();
  lcd_dc_data();
  for (size_t i = 0; i < len; i++)
    pio_spi_write8(data[i]);
  lcd_spi_wait_idle();
  lcd_cs_high();
}

static void lcd_write_byte(uint8_t b) { lcd_write_data(&b, 1); }

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  lcd_cs_low();

  lcd_dc_cmd();
  pio_spi_write8(ST7365P_CASET);
  lcd_spi_wait_idle();

  lcd_dc_data();
  pio_spi_write8(x0 >> 8);
  pio_spi_write8(x0 & 0xFF);
  pio_spi_write8(x1 >> 8);
  pio_spi_write8(x1 & 0xFF);
  lcd_spi_wait_idle();

  lcd_dc_cmd();
  pio_spi_write8(ST7365P_RASET);
  lcd_spi_wait_idle();

  lcd_dc_data();
  pio_spi_write8(y0 >> 8);
  pio_spi_write8(y0 & 0xFF);
  pio_spi_write8(y1 >> 8);
  pio_spi_write8(y1 & 0xFF);
  lcd_spi_wait_idle();

  lcd_dc_cmd();
  pio_spi_write8(ST7365P_RAMWR);
  lcd_spi_wait_idle();

  lcd_cs_high();
}

// ── Init sequence
// ───────────────────────────────────────────────────────────── Ported directly
// from the working constellation-pico Rust project (st7789.rs). No
// ST7796S-style 0xF0 manufacturer unlock — the panel responds to the standard
// ST7789 init sequence. Backlight is controlled by the STM32 keyboard MCU.

void display_init(void) {
  // Initialize PIO for SPI master
  uint offset = pio_add_program(LCD_PIO, &lcd_spi_program);
  pio_sm_config cfg_pio = lcd_spi_program_get_default_config(offset);
  sm_config_set_out_pins(&cfg_pio, LCD_PIN_MOSI, 1);
  sm_config_set_sideset_pins(&cfg_pio, LCD_PIN_SCK);
  // Disable auto-pull. Manual PULL fetches new word. Shift from MSB.
  sm_config_set_out_shift(&cfg_pio, false, false, 32);
  sm_config_set_fifo_join(&cfg_pio, PIO_FIFO_JOIN_TX);
  float clkdiv = (float)clock_get_hz(clk_sys) / (LCD_SPI_BAUD * 2);
  sm_config_set_clkdiv(&cfg_pio, clkdiv);

  s_pio_sm = pio_claim_unused_sm(LCD_PIO, true);
  pio_sm_init(LCD_PIO, s_pio_sm, offset, &cfg_pio);
  pio_sm_set_pins_with_mask(LCD_PIO, s_pio_sm, (1u << LCD_PIN_SCK),
                            (1u << LCD_PIN_SCK) | (1u << LCD_PIN_MOSI));
  pio_sm_set_pindirs_with_mask(LCD_PIO, s_pio_sm,
                               (1u << LCD_PIN_SCK) | (1u << LCD_PIN_MOSI),
                               (1u << LCD_PIN_SCK) | (1u << LCD_PIN_MOSI));
  pio_gpio_init(LCD_PIO, LCD_PIN_MOSI);
  pio_gpio_init(LCD_PIO, LCD_PIN_SCK);

  pio_sm_set_enabled(LCD_PIO, s_pio_sm, true);
  printf("[LCD] PIO SPI initialized (req baud: %lu)\n",
         (unsigned long)LCD_SPI_BAUD);

  // Control pins as GPIO outputs
  gpio_init(LCD_PIN_CS);
  gpio_set_dir(LCD_PIN_CS, GPIO_OUT);
  lcd_cs_high();
  gpio_init(LCD_PIN_DC);
  gpio_set_dir(LCD_PIN_DC, GPIO_OUT);
  lcd_dc_cmd();
  gpio_init(LCD_PIN_RST);
  gpio_set_dir(LCD_PIN_RST, GPIO_OUT);

  // Hardware reset — pulse RST low for 10 ms, then high, wait 120 ms
  gpio_put(LCD_PIN_RST, 0);
  sleep_ms(10);
  gpio_put(LCD_PIN_RST, 1);
  sleep_ms(120);

  printf("[LCD] init start\n");

  // Software reset — wait 10 ms before further commands
  lcd_write_cmd(ST7365P_SWRST);
  sleep_ms(10);

  // Colour mode: 16-bit RGB565 (COLMOD)
  lcd_write_cmd(ST7365P_COLMOD);
  lcd_write_byte(0x55);

  // Memory access control: 0x48 (MADCTL_MX | MADCTL_BGR, matches Picoware)
  lcd_write_cmd(ST7365P_MADCTL);
  lcd_write_byte(0x48);

  // Inversion on (INVON)
  lcd_write_cmd(ST7365P_INVON);

  // Entry mode set (EMS, 0xB7)
  // Maps 16-bit to 18-bit color conversion correctly for ST7789
  lcd_write_cmd(0xB7);
  lcd_write_byte(0xC6);

  // Sleep out — display enters normal operation, wait 10 ms
  lcd_write_cmd(ST7365P_SLPOUT);
  sleep_ms(10);

  // Display on (DISPON)
  lcd_write_cmd(ST7365P_DISPON);

  printf("[LCD] init done, flushing black fill...\n");

  // DMA channel for framebuffer flushes.
  // Must use DMA_SIZE_8 with the PIO SPI to send bytes sequentially.
  // By writing to `&LCD_PIO->txf[s_pio_sm] + 3`, we place each byte in the
  // MSB of the TX FIFO, where our PIO program expects it.
  s_dma_chan = dma_claim_unused_channel(true);
  dma_channel_config cfg = dma_channel_get_default_config(s_dma_chan);
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
  channel_config_set_dreq(&cfg, pio_get_dreq(LCD_PIO, s_pio_sm, true));
  channel_config_set_read_increment(&cfg, true);
  channel_config_set_write_increment(&cfg, false);
  dma_channel_configure(s_dma_chan, &cfg,
                        ((volatile uint8_t *)&LCD_PIO->txf[s_pio_sm]) + 3,
                        s_framebuffer,
                        FB_SIZE, // FB_WIDTH * FB_HEIGHT * 2 bytes
                        false);

  // Fill black to confirm flush path works
  display_clear(COLOR_BLACK);
  display_flush();
  printf("[LCD] flush done\n");
}

void display_deinit(void) {
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }
  if (s_dma_chan >= 0)
    dma_channel_unclaim(s_dma_chan);
  pio_sm_set_enabled(LCD_PIO, s_pio_sm, false);
}

// ── Drawing functions
// ─────────────────────────────────────────────────────────

void display_clear(uint16_t color) {
  // Big-endian swap: ST7365P wants bytes big-endian over SPI
  uint16_t be = (color >> 8) | (color << 8);

  // Fast path: use memset for black (0x0000) or white (0xFFFF)
  if (be == 0x0000 || be == 0xFFFF) {
    memset(s_framebuffer, be & 0xFF, FB_SIZE);
    return;
  }

  // Optimized: fill in 32-pixel chunks for better memory bandwidth
  uint32_t color32 = ((uint32_t)be << 16) | be; // Two pixels
  uint32_t *fb32 = (uint32_t *)s_framebuffer;
  size_t count32 = (FB_WIDTH * FB_HEIGHT) / 2; // Number of 32-bit words

  for (size_t i = 0; i < count32; i++) {
    fb32[i] = color32;
  }
}

void display_set_pixel(int x, int y, uint16_t color) {
  if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT)
    return;
  uint16_t be = (color >> 8) | (color << 8);
  s_framebuffer[y * FB_WIDTH + x] = be;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > FB_WIDTH)
    w = FB_WIDTH - x;
  if (y + h > FB_HEIGHT)
    h = FB_HEIGHT - y;
  if (w <= 0 || h <= 0)
    return;

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
    for (int col = 0; col < w; col++)
      p[col] = be;
  }
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color) {
  display_fill_rect(x, y, w, 1, color);
  display_fill_rect(x, y + h - 1, w, 1, color);
  display_fill_rect(x, y, 1, h, color);
  display_fill_rect(x + w - 1, y, 1, h, color);
}

void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
  // Bresenham's line algorithm
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  while (true) {
    display_set_pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

int display_draw_text(int x, int y, const char *text, uint16_t fg,
                      uint16_t bg) {
  int start_x = x;
  uint16_t fg_be = (fg >> 8) | (fg << 8);
  uint16_t bg_be = (bg >> 8) | (bg << 8);

  while (*text) {
    char c = *text++;
    if (c < 0x20 || c > 0x7E)
      c = '?';
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
  while (*text++)
    len++;
  return len * FONT_W;
}

void display_draw_image(int x, int y, int w, int h, const uint16_t *data) {
  for (int row = 0; row < h; row++) {
    int py = y + row;
    if (py < 0 || py >= FB_HEIGHT)
      continue;
    for (int col = 0; col < w; col++) {
      int px = x + col;
      if (px < 0 || px >= FB_WIDTH)
        continue;
      uint16_t c = data[row * w + col];
      s_framebuffer[py * FB_WIDTH + px] = (c >> 8) | (c << 8);
    }
  }
}

void display_draw_image_partial(int x, int y, int img_w, int img_h,
                                const uint16_t *data, int sx, int sy, int sw,
                                int sh, bool flip_x, bool flip_y) {
  if (sx < 0) {
    sw += sx;
    sx = 0;
  }
  if (sy < 0) {
    sh += sy;
    sy = 0;
  }
  if (sx + sw > img_w)
    sw = img_w - sx;
  if (sy + sh > img_h)
    sh = img_h - sy;

  if (sw <= 0 || sh <= 0)
    return;

  for (int row = 0; row < sh; row++) {
    int py = y + row;
    if (py < 0 || py >= FB_HEIGHT)
      continue;

    int src_row = flip_y ? (sy + sh - 1 - row) : (sy + row);

    for (int col = 0; col < sw; col++) {
      int px = x + col;
      if (px < 0 || px >= FB_WIDTH)
        continue;

      int src_col = flip_x ? (sx + sw - 1 - col) : (sx + col);
      uint16_t c = data[src_row * img_w + src_col];

      s_framebuffer[py * FB_WIDTH + px] = (c >> 8) | (c << 8);
    }
  }
}

void display_draw_image_scaled(int x, int y, int img_w, int img_h,
                               const uint16_t *data, float scale, float angle) {
  // tgx_draw_image_scaled renders directly into the framebuffer using TGX's
  // native RGB565 format (little-endian).  Our framebuffer stores pixels
  // byte-swapped (big-endian) for the 8-bit DMA path, so we need to:
  //   1. Byte-swap the affected region to native LE so TGX math is correct.
  //   2. Let TGX render.
  //   3. Byte-swap the affected region back to BE for the DMA flush.

  // Calculate bounding box of transformed image to only swap affected region.
  // Image is centered at (x, y). Transform corners, find min/max.
  float cosa = cosf(angle);
  float sina = sinf(angle);
  float w = img_w * scale * 0.5f;
  float h = img_h * scale * 0.5f;

  // Transform all 4 corners and track min/max
  float corners[4][2] = {
    {-w, -h}, {w, -h}, {w, h}, {-w, h}
  };
  float min_x = x, max_x = x, min_y = y, max_y = y;
  for (int i = 0; i < 4; i++) {
    float tx = corners[i][0] * cosa - corners[i][1] * sina + x;
    float ty = corners[i][0] * sina + corners[i][1] * cosa + y;
    if (tx < min_x) min_x = tx;
    if (tx > max_x) max_x = tx;
    if (ty < min_y) min_y = ty;
    if (ty > max_y) max_y = ty;
  }

  // Clamp to framebuffer bounds
  int bx = (int)floorf(min_x);
  int by = (int)floorf(min_y);
  int bw = (int)ceilf(max_x - min_x);
  int bh = (int)ceilf(max_y - min_y);
  if (bx < 0) bx = 0;
  if (by < 0) by = 0;
  if (bx + bw > FB_WIDTH) bw = FB_WIDTH - bx;
  if (by + bh > FB_HEIGHT) bh = FB_HEIGHT - by;
  if (bw <= 0 || bh <= 0) return;

  // Byte-swap only the affected region
  uint16_t *fb = s_framebuffer;
  for (int row = by; row < by + bh; row++) {
    for (int col = bx; col < bx + bw; col++) {
      fb[row * FB_WIDTH + col] = (fb[row * FB_WIDTH + col] >> 8) | (fb[row * FB_WIDTH + col] << 8);
    }
  }

  tgx_draw_image_scaled(fb, FB_WIDTH, FB_HEIGHT, data, img_w, img_h, x, y,
                        scale, angle);

  // Byte-swap back only the affected region
  for (int row = by; row < by + bh; row++) {
    for (int col = bx; col < bx + bw; col++) {
      fb[row * FB_WIDTH + col] = (fb[row * FB_WIDTH + col] >> 8) | (fb[row * FB_WIDTH + col] << 8);
    }
  }
}

void display_flush(void) {
  if (s_dma_active) {
    // Wait for previous DMA completion
    dma_channel_wait_for_finish_blocking(s_dma_chan);

    // Ensure state machine is fully drained before deasserting CS
    lcd_spi_wait_idle();

    lcd_cs_high();
  }

  // Swap buffers
  int front_buffer_idx = s_back_buffer_idx;
  s_back_buffer_idx = 1 - s_back_buffer_idx;
  s_framebuffer = s_framebuffers[s_back_buffer_idx];

  lcd_set_window(0, 0, FB_WIDTH - 1, FB_HEIGHT - 1);

  lcd_cs_low();
  lcd_dc_data();

  // DMA transfer: non-blocking, CPU-free framebuffer → SPI
  dma_channel_set_read_addr(s_dma_chan, s_framebuffers[front_buffer_idx],
                            false);
  dma_channel_set_trans_count(s_dma_chan, FB_SIZE, true); // start transfer
  s_dma_active = true;
}

void display_set_brightness(uint8_t brightness) {
  // Backlight is controlled by the STM32 keyboard MCU (kbd_set_backlight).
  // This function is a no-op on PicoCalc v2.0.
  (void)brightness;
}

void display_darken(void) {
  // If a DMA transfer is in progress, wait for it to finish and end the SPI
  // transaction before we read the front buffer.
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }

  // With double buffering, we want to create a stable, darkened snapshot of the
  // currently visible screen (the front buffer) and place it into BOTH buffers.
  // This ensures that when an overlay app (like the system menu) redraws and
  // flushes, it doesn't alternate between a darkened and a bright background.
  uint16_t *front = s_framebuffers[1 - s_back_buffer_idx];
  uint16_t *back = s_framebuffers[s_back_buffer_idx];
  size_t n = (FB_WIDTH * FB_HEIGHT);

  for (size_t i = 0; i < n; i++) {
    uint16_t p = front[i];
    uint16_t native = (p >> 8) | (p << 8); // unswap
    native = (native >> 1) &
             0xFBEF; // darken (clear bits that shift across channels)
    uint16_t darkened = (native >> 8) | (native << 8); // reswap
    front[i] = darkened;
    back[i] = darkened;
  }
}

const uint16_t *display_get_framebuffer(void) { return s_framebuffer; }

const uint16_t *display_get_front_buffer(void) {
  return s_framebuffers[1 - s_back_buffer_idx];
}

const uint16_t *display_get_screen_buffer(void) {
  if (s_dma_active) {
    dma_channel_wait_for_finish_blocking(s_dma_chan);
    lcd_spi_wait_idle();
    lcd_cs_high();
    s_dma_active = false;
  }
  return display_get_front_buffer();
}
