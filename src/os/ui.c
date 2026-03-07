#include "ui.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/wifi.h"
#include "../splash_logo.h"
#include "clock.h"
#include "os.h"    // BTN_* constants
#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#define C_HEADER_BG RGB565(20, 20, 60)
#define C_TEXT COLOR_WHITE
#define C_TEXT_DIM COLOR_GRAY
#define C_BATTERY_OK COLOR_GREEN
#define C_BATTERY_LO COLOR_RED
#define C_BORDER RGB565(60, 60, 100)

static int s_last_bat = -999;
static int s_last_wifi_status = -1;
static char s_last_clock[16] = "";

bool ui_needs_header_redraw(void) {
  if (kbd_get_battery_percent() != s_last_bat)
    return true;
  int cur_wifi = wifi_is_available() ? (int)wifi_get_status() : -1;
  if (cur_wifi != s_last_wifi_status)
    return true;

  if (clock_is_set()) {
    char clk_buf[16];
    clock_format(clk_buf, sizeof(clk_buf));
    if (strcmp(clk_buf, s_last_clock) != 0)
      return true;
  }
  return false;
}

void ui_draw_header(const char *title) {
  display_fill_rect(0, 0, FB_WIDTH, 28, C_HEADER_BG);
  display_draw_text(8, 8, title ? title : "", C_TEXT, C_HEADER_BG);

  // Right-side status: lay out right-to-left
  int x = FB_WIDTH - 8;

  // 1. Battery (rightmost)
  int bat = kbd_get_battery_percent();
  s_last_bat = bat;
  if (bat >= 0) {
    char bat_buf[16];
    snprintf(bat_buf, sizeof(bat_buf), "Bat:%d%%", bat);
    int bat_w = (int)strlen(bat_buf) * 6;
    x -= bat_w;
    uint16_t c = (bat > 20) ? C_BATTERY_OK : C_BATTERY_LO;
    display_draw_text(x, 8, bat_buf, c, C_HEADER_BG);
    x -= 12;
  }

  // 2. WiFi
  s_last_wifi_status = wifi_is_available() ? (int)wifi_get_status() : -1;
  if (wifi_is_available()) {
    wifi_status_t status = wifi_get_status();
    const char *icon = (status == WIFI_STATUS_CONNECTED) ? "WiFi" : "WiFi!";
    uint16_t c =
        (status == WIFI_STATUS_CONNECTED) ? C_BATTERY_OK : C_BATTERY_LO;

    int icon_w = (int)strlen(icon) * 6;
    x -= icon_w;
    display_draw_text(x, 8, icon, c, C_HEADER_BG);
    x -= 12;
  }

  // 3. Clock
  if (clock_is_set()) {
    char clk_buf[16];
    clock_format(clk_buf, sizeof(clk_buf));
    strncpy(s_last_clock, clk_buf, sizeof(s_last_clock));
    int clk_w = (int)strlen(clk_buf) * 6;
    x -= clk_w;
    display_draw_text(x, 8, clk_buf, C_TEXT, C_HEADER_BG);
  }

  display_fill_rect(0, 28, FB_WIDTH, 1, C_BORDER);
}

void ui_draw_footer(const char *left_text, const char *right_text) {
  display_fill_rect(0, FB_HEIGHT - 18, FB_WIDTH, 18, C_HEADER_BG);
  display_fill_rect(0, FB_HEIGHT - 18, FB_WIDTH, 1, C_BORDER);

  if (left_text && left_text[0]) {
    display_draw_text(8, FB_HEIGHT - 13, left_text, C_TEXT_DIM, C_HEADER_BG);
  }

  if (right_text && right_text[0]) {
    int w = display_text_width(right_text);
    display_draw_text(FB_WIDTH - 8 - w, FB_HEIGHT - 13, right_text, C_TEXT_DIM,
                      C_HEADER_BG);
  }
}

int ui_draw_tabs(const char **tabs, int count, int active_index, int y) {
  if (count <= 0 || !tabs)
    return 0;

  // Clamp active_index to valid range
  if (active_index < 0)
    active_index = 0;
  if (active_index >= count)
    active_index = count - 1;

  const int TAB_HEIGHT = 20;
  const int TAB_PADDING = 8;
  const int TAB_SPACING = 4;

  // Draw background and border
  display_fill_rect(0, y, FB_WIDTH, TAB_HEIGHT, C_HEADER_BG);
  display_fill_rect(0, y + TAB_HEIGHT, FB_WIDTH, 1, C_BORDER);

  // Calculate tab widths (distribute evenly)
  int available_width = FB_WIDTH - (TAB_PADDING * 2) - (TAB_SPACING * (count - 1));
  int tab_width = available_width / count;

  // Draw each tab
  int x = TAB_PADDING;
  for (int i = 0; i < count; i++) {
    if (!tabs[i])
      continue;

    bool is_active = (i == active_index);
    uint16_t text_color = is_active ? C_TEXT : C_TEXT_DIM;
    
    // Draw active tab background highlight
    if (is_active) {
      display_fill_rect(x - 2, y + 2, tab_width + 4, TAB_HEIGHT - 4, 
                        RGB565(40, 40, 80));
    }

    // Center text within tab
    int text_w = display_text_width(tabs[i]);
    int text_x = x + (tab_width - text_w) / 2;
    int text_y = y + (TAB_HEIGHT - 8) / 2;
    
    display_draw_text(text_x, text_y, tabs[i], text_color, C_HEADER_BG);

    x += tab_width + TAB_SPACING;
  }

  return TAB_HEIGHT + 1; // Return total height including border
}

void ui_draw_splash(const char *status, const char *subtext) {
  display_clear(COLOR_BLACK);

#if LOGO_W > 0 && LOGO_H > 0
  // ── Logo ──────────────────────────────────────────────────────────────────
  int lx = (FB_WIDTH - LOGO_W) / 2;
  int ly = (FB_HEIGHT - LOGO_H) / 2 - 16;
  display_draw_image(lx, ly, LOGO_W, LOGO_H, logo_data);

  // ── Status text below logo ─────────────────────────────────────────────
  if (status && status[0]) {
    int sx = (FB_WIDTH - display_text_width(status)) / 2;
    display_draw_text(sx, ly + LOGO_H + 12, status, COLOR_GRAY, COLOR_BLACK);
  }
  if (subtext && subtext[0]) {
    int s2x = (FB_WIDTH - display_text_width(subtext)) / 2;
    display_draw_text(s2x, ly + LOGO_H + 24, subtext, COLOR_GRAY, COLOR_BLACK);
  }
#else
  // ── No logo: centred title + status ───────────────────────────────────────
  const char *title = "PicOS";
  int tx = (FB_WIDTH - display_text_width(title)) / 2;
  display_draw_text(tx, FB_HEIGHT / 2 - 14, title, COLOR_WHITE, COLOR_BLACK);

  if (status && status[0]) {
    int sx = (FB_WIDTH - display_text_width(status)) / 2;
    display_draw_text(sx, FB_HEIGHT / 2 + 2, status, COLOR_GRAY, COLOR_BLACK);
  }
  if (subtext && subtext[0]) {
    int s2x = (FB_WIDTH - display_text_width(subtext)) / 2;
    display_draw_text(s2x, FB_HEIGHT / 2 + 14, subtext, COLOR_GRAY,
                      COLOR_BLACK);
  }
#endif

  display_flush();
}

// ── Shared dialog style ───────────────────────────────────────────────────────

#define DLG_BG      RGB565(20, 28, 50)
#define DLG_BORDER  RGB565(80, 100, 150)
#define DLG_FIELD   RGB565(10, 14, 30)
#define DLG_DIM     RGB565(100, 100, 100)
#define DLG_W       280
#define DLG_X       ((FB_WIDTH  - DLG_W) / 2)

// Draw a wrapped message into a dialog box at (x, y_start).
// Returns the y coordinate after the last line drawn.
static int dlg_draw_message(int x, int y, int w, const char *msg, uint16_t bg) {
  if (!msg || !msg[0]) return y;
  const int COLS = w / 6; // chars that fit (6px font)
  while (*msg) {
    int avail = strlen(msg);
    int take  = avail > COLS ? COLS : avail;
    // Try to break at a word boundary
    if (avail > COLS) {
      for (int k = take - 1; k > 0; k--) {
        if (msg[k] == ' ' || msg[k] == '/') { take = k; break; }
      }
    }
    char line[64];
    int n = take < (int)sizeof(line) - 1 ? take : (int)sizeof(line) - 1;
    memcpy(line, msg, n);
    line[n] = '\0';
    display_draw_text(x, y, line, COLOR_WHITE, bg);
    y += 12;
    msg += take;
    if (*msg == ' ') msg++; // skip leading space on next line
  }
  return y;
}

// ── ui_text_input ─────────────────────────────────────────────────────────────

bool ui_text_input(const char *prompt, const char *default_val,
                   char *out_buf, int out_len) {
  // Layout
  const int DH = 90;
  const int DY = (FB_HEIGHT - DH) / 2;
  const int FX = DLG_X + 8;
  const int FY = DY + 30;
  const int FW = DLG_W - 16;
  const int FH = 18;

  // Draw dialog (once)
  display_darken();
  display_fill_rect(DLG_X, DY, DLG_W, DH, DLG_BG);
  display_draw_rect(DLG_X, DY, DLG_W, DH, DLG_BORDER);
  display_draw_text(DLG_X + 10, DY + 10,
                    prompt ? prompt : "Input:", COLOR_WHITE, DLG_BG);
  display_fill_rect(FX, FY, FW, FH, DLG_FIELD);
  display_draw_rect(FX, FY, FW, FH, DLG_BORDER);
  const char *hint = "Enter=OK  Esc=Cancel";
  int hw = display_text_width(hint);
  display_draw_text(DLG_X + (DLG_W - hw) / 2, DY + DH - 14,
                    hint, DLG_DIM, DLG_BG);
  display_flush();

  // Input buffer
  const int MAX_CHARS = 127;
  char buf[128] = "";
  int len = 0;
  if (default_val) {
    strncpy(buf, default_val, MAX_CHARS);
    buf[MAX_CHARS] = '\0';
    len = strlen(buf);
  }

  bool accepted = false;
  bool dirty    = true;
  while (true) {
    kbd_poll();
    uint32_t btns = kbd_get_buttons_pressed();
    char c        = kbd_get_char();

    if (btns & BTN_ESC) break;
    if (c == '\n') { accepted = true; break; }  // Enter = 0x0A
    if (c == '\b') {
      if (len > 0) { len--; dirty = true; }
    } else if (c >= 32 && c < 127 && len < MAX_CHARS) {
      buf[len++] = c;
      dirty = true;
    }
    buf[len] = '\0';

    if (dirty) {
      // Redraw only the input field interior
      display_fill_rect(FX + 1, FY + 1, FW - 2, FH - 2, DLG_FIELD);
      // Scroll: show only the last N chars that fit
      const int VCOLS = (FW - 8) / 6;
      const char *view = (len > VCOLS) ? (buf + len - VCOLS) : buf;
      char dbuf[66];
      snprintf(dbuf, sizeof(dbuf), "%.*s_", VCOLS, view);
      display_draw_text(FX + 4, FY + 5, dbuf, COLOR_WHITE, DLG_FIELD);
      display_flush();
      dirty = false;
    }
    sleep_ms(20);
  }

  if (accepted && out_buf && out_len > 0) {
    strncpy(out_buf, buf, out_len - 1);
    out_buf[out_len - 1] = '\0';
  }
  return accepted;
}

// ── ui_confirm ────────────────────────────────────────────────────────────────

bool ui_confirm(const char *message) {
  // Measure how many lines the message needs (up to 2)
  const int COLS = (DLG_W - 20) / 6;
  int msg_len = message ? strlen(message) : 0;
  int lines   = msg_len == 0 ? 1 : (msg_len + COLS - 1) / COLS;
  if (lines > 2) lines = 2;

  const int DH = 44 + lines * 12 + 18; // border + text lines + hint
  const int DY = (FB_HEIGHT - DH) / 2;

  display_darken();
  display_fill_rect(DLG_X, DY, DLG_W, DH, DLG_BG);
  display_draw_rect(DLG_X, DY, DLG_W, DH, DLG_BORDER);

  dlg_draw_message(DLG_X + 10, DY + 14, DLG_W - 20,
                   message ? message : "Are you sure?", DLG_BG);

  const char *hint = "Enter=Yes  Esc=No";
  int hw = display_text_width(hint);
  display_draw_text(DLG_X + (DLG_W - hw) / 2, DY + DH - 14,
                    hint, DLG_DIM, DLG_BG);
  display_flush();

  while (true) {
    kbd_poll();
    uint32_t btns = kbd_get_buttons_pressed();
    char c        = kbd_get_char();
    if (btns & BTN_ESC || c == 'n' || c == 'N') return false;
    if (c == '\n'       || c == 'y' || c == 'Y') return true;
    sleep_ms(20);
  }
}
