#include "ui.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/wifi.h"
#include "../splash_logo.h"
#include "clock.h"

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
