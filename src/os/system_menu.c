#include "system_menu.h"
#include "os.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"

#include "lua.h"
#include "lauxlib.h"

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include <string.h>
#include <stdio.h>

// ── App-registered items ──────────────────────────────────────────────────────

typedef struct {
    char  label[32];
    void (*callback)(void *user);
    void *user;
} app_item_t;

static app_item_t s_app_items[SYSMENU_MAX_APP_ITEMS];
static int        s_app_item_count = 0;
static uint8_t    s_brightness = 128;

// ── Visual constants ──────────────────────────────────────────────────────────

#define PANEL_W    200
#define TITLE_H     16   // title bar height (px)
#define ITEM_H      13   // per-item row height (px): 8px font + 5px padding
#define FOOTER_H    12   // footer hint bar height (px)

#define C_PANEL_BG   RGB565(20,  28,  50)
#define C_TITLE_BG   RGB565(10,  14,  30)
#define C_SEL_BG     RGB565(40,  80, 160)
#define C_BORDER     RGB565(80, 100, 150)

// ── Flat item list types ──────────────────────────────────────────────────────

typedef enum {
    ITEM_APP_CB = 0,
    ITEM_BRIGHTNESS,
    ITEM_BATTERY,
    ITEM_WIFI,
    ITEM_REBOOT,
    ITEM_EXIT,
} item_type_t;

typedef struct {
    item_type_t type;
    int         app_idx;  // valid only when type == ITEM_APP_CB
} flat_item_t;

// ── Panel drawing ─────────────────────────────────────────────────────────────

static void draw_panel(const flat_item_t *items, int count, int sel,
                       int px, int py, int ph, int bat) {
    // Outer border
    display_draw_rect(px, py, PANEL_W, ph, C_BORDER);

    // Title bar
    display_fill_rect(px + 1, py + 1, PANEL_W - 2, TITLE_H, C_TITLE_BG);
    const char *title = "System Menu";
    int tw = display_text_width(title);
    display_draw_text(px + (PANEL_W - tw) / 2, py + 5,
                      title, COLOR_WHITE, C_TITLE_BG);

    // Divider after title
    display_fill_rect(px + 1, py + 1 + TITLE_H, PANEL_W - 2, 1, C_BORDER);

    // Items
    int items_y = py + 1 + TITLE_H + 1;
    for (int i = 0; i < count; i++) {
        int iy = items_y + i * ITEM_H;
        bool selected = (i == sel);
        uint16_t bg = selected ? C_SEL_BG : C_PANEL_BG;
        display_fill_rect(px + 1, iy, PANEL_W - 2, ITEM_H, bg);

        char label[34];
        uint16_t fg = COLOR_WHITE;

        switch (items[i].type) {
            case ITEM_APP_CB:
                snprintf(label, sizeof(label), "%s",
                         s_app_items[items[i].app_idx].label);
                break;
            case ITEM_BRIGHTNESS:
                snprintf(label, sizeof(label), "Brightness: %d <>",
                         s_brightness);
                break;
            case ITEM_BATTERY:
                if (bat >= 0)
                    snprintf(label, sizeof(label), "Battery: %d%%", bat);
                else
                    snprintf(label, sizeof(label), "Battery: N/A");
                fg = (bat > 20) ? COLOR_GREEN : COLOR_RED;
                break;
            case ITEM_WIFI:
                snprintf(label, sizeof(label), "WiFi: Not Available");
                fg = COLOR_GRAY;
                break;
            case ITEM_REBOOT:
                snprintf(label, sizeof(label), "Reboot");
                fg = selected ? COLOR_WHITE : COLOR_RED;
                break;
            case ITEM_EXIT:
                snprintf(label, sizeof(label), "Exit App");
                fg = selected ? COLOR_WHITE : COLOR_YELLOW;
                break;
        }

        // Selection indicator and item text
        display_draw_text(px + 4,  iy + 2, selected ? ">" : " ",
                          COLOR_WHITE, bg);
        display_draw_text(px + 10, iy + 2, label, fg, bg);
    }

    // Divider before footer
    int footer_div_y = items_y + count * ITEM_H;
    display_fill_rect(px + 1, footer_div_y, PANEL_W - 2, 1, C_BORDER);

    // Footer hint
    int footer_y = footer_div_y + 1;
    display_fill_rect(px + 1, footer_y, PANEL_W - 2, FOOTER_H, C_TITLE_BG);
    display_draw_text(px + 4, footer_y + 2,
                      "Enter:select  Esc:close", COLOR_GRAY, C_TITLE_BG);
}

// ── Public API ────────────────────────────────────────────────────────────────

void system_menu_init(void) {
    s_app_item_count = 0;
    s_brightness = 128;
}

void system_menu_add_item(const char *label,
                          void (*callback)(void *user), void *user) {
    if (s_app_item_count >= SYSMENU_MAX_APP_ITEMS) return;
    app_item_t *it = &s_app_items[s_app_item_count++];
    strncpy(it->label, label, sizeof(it->label) - 1);
    it->label[sizeof(it->label) - 1] = '\0';
    it->callback = callback;
    it->user     = user;
}

void system_menu_clear_items(void) {
    s_app_item_count = 0;
}

void system_menu_show(lua_State *L) {
    // Build flat item list: app items first, then built-ins
    flat_item_t items[SYSMENU_MAX_APP_ITEMS + 5];
    int count = 0;

    for (int i = 0; i < s_app_item_count; i++) {
        items[count].type    = ITEM_APP_CB;
        items[count].app_idx = i;
        count++;
    }
    items[count++] = (flat_item_t){ ITEM_BRIGHTNESS, 0 };
    items[count++] = (flat_item_t){ ITEM_BATTERY,    0 };
    items[count++] = (flat_item_t){ ITEM_WIFI,       0 };
    items[count++] = (flat_item_t){ ITEM_REBOOT,     0 };
    items[count++] = (flat_item_t){ ITEM_EXIT,       0 };

    // panel_h = border(1) + title(16) + divider(1) + items(count*13)
    //         + divider(1) + footer(12) + border(1) = 32 + count*13
    int panel_h = 32 + count * ITEM_H;
    int panel_x = (FB_WIDTH  - PANEL_W) / 2;
    int panel_y = (FB_HEIGHT - panel_h) / 2;

    // Read battery once — avoids an I2C hit on every panel redraw.
    int bat = kbd_get_battery_percent();

    // Darken the current framebuffer for the overlay effect
    display_darken();

    int  sel         = 0;
    bool running     = true;
    bool need_redraw = true;

    while (running) {
        if (need_redraw) {
            draw_panel(items, count, sel, panel_x, panel_y, panel_h, bat);
            display_flush();
            need_redraw = false;
        }

        kbd_poll();
        uint32_t pressed = kbd_get_buttons_pressed();

        if (pressed & BTN_UP) {
            if (sel > 0) { sel--; need_redraw = true; }
        }
        if (pressed & BTN_DOWN) {
            if (sel < count - 1) { sel++; need_redraw = true; }
        }

        // Left / Right: adjust brightness when on Brightness item
        if ((pressed & BTN_LEFT) && items[sel].type == ITEM_BRIGHTNESS) {
            s_brightness = (s_brightness >= 16) ? s_brightness - 16 : 0;
            kbd_set_backlight(s_brightness);
            need_redraw = true;
        }
        if ((pressed & BTN_RIGHT) && items[sel].type == ITEM_BRIGHTNESS) {
            s_brightness = (s_brightness <= 239) ? s_brightness + 16 : 255;
            kbd_set_backlight(s_brightness);
            need_redraw = true;
        }

        if (pressed & BTN_ENTER) {
            switch (items[sel].type) {
                case ITEM_APP_CB:
                    // Call the registered callback then dismiss
                    s_app_items[items[sel].app_idx].callback(
                        s_app_items[items[sel].app_idx].user);
                    running = false;
                    break;
                case ITEM_BRIGHTNESS:
                    // Enter increments; wraps 255→0
                    s_brightness = (s_brightness <= 239) ? s_brightness + 16 : 0;
                    kbd_set_backlight(s_brightness);
                    need_redraw = true;
                    break;
                case ITEM_BATTERY:
                case ITEM_WIFI:
                    // Read-only items — do nothing
                    break;
                case ITEM_REBOOT:
                    watchdog_enable(1, true);
                    for (;;) tight_loop_contents();
                    break; /* unreachable */
                case ITEM_EXIT:
                    system_menu_clear_items();
                    luaL_error(L, "__picocalc_exit__"); /* does longjmp */
                    break; /* unreachable */
            }
        }

        if (pressed & BTN_ESC) running = false;

        sleep_ms(16);
    }
    // Return normally — the Lua hook returns, Lua execution resumes
}
