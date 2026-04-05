#include "launcher.h"
#include "launcher_types.h"
#include "app_runner.h"
#include "lua_runner.h"
#include "native_loader.h"
#include "../drivers/audio.h"
#include "../drivers/display.h"
#include "../drivers/image_api.h"
#include "../drivers/keyboard.h"
#include "../drivers/mp3_player.h"
#include "../drivers/sdcard.h"
#include "../drivers/wifi.h"

#include "clock.h"
#include "config.h"
#include "lua_psram_alloc.h"
#include "screenshot.h"
#include "system_menu.h"
#include "ui.h"
#include "umm_malloc.h"

#include <stdatomic.h>

#include "../dev_commands.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ── App discovery
// ─────────────────────────────────────────────────────────────

#define MAX_APPS 32

static app_entry_t *s_apps = NULL;
static int s_app_count = 0;

// Tiny JSON parser — just enough to pull "name", "description", "version"
// from a simple flat JSON object.  Not a full parser.
static bool json_get_string(const char *json, const char *key, char *out,
                            int out_len) {
  char search[64];
  snprintf(search, sizeof(search), "\"%s\"", key);
  const char *p = json;
  while ((p = strstr(p, search)) != NULL) {
    const char *q = p + strlen(search);
    while (*q == ' ' || *q == ':' || *q == '\t')
      q++;
    if (*q == '"') {
      q++; // skip opening quote
      int i = 0;
      while (*q && *q != '"' && i < out_len - 1)
        out[i++] = *q++;
      out[i] = '\0';
      return true;
    }
    p++; // false match (key name inside a value), keep searching
  }
  return false;
}

static bool json_get_int(const char *json, const char *key, uint32_t *out) {
  char search[64];
  snprintf(search, sizeof(search), "\"%s\"", key);
  const char *p = strstr(json, search);
  if (!p)
    return false;
  p += strlen(search);
  while (*p == ' ' || *p == ':' || *p == '\t')
    p++;
  *out = (uint32_t)atoi(p);
  return true;
}

static bool json_has_requirement(const char *json, const char *requirement) {
  const char *p = strstr(json, "\"requirements\"");
  if (!p)
    return false;
  while (*p && *p != '[')
    p++;
  if (*p != '[')
    return false;

  char search[96];
  snprintf(search, sizeof(search), "\"%s\"", requirement);
  const char *bracket_start = p;

  while (*p && *p != ']') {
    if (strstr(p, search)) {
      const char *found      = strstr(p, search);
      const char *bracket_end = strchr(bracket_start, ']');
      if (found < bracket_end)
        return true;
    }
    p++;
  }
  return false;
}

static void on_app_dir(const sdcard_entry_t *entry, void *user) {
  (void)user;
  if (!entry->is_dir)
    return;
  if (entry->name[0] == '.')
    return;
  if (s_app_count >= MAX_APPS)
    return;

  // Detect available runtimes
  char lua_path[160], elf_path[160];
  snprintf(lua_path, sizeof(lua_path), "/apps/%s/main.lua", entry->name);
  snprintf(elf_path, sizeof(elf_path), "/apps/%s/main.elf", entry->name);
  bool has_lua = sdcard_fexists(lua_path);
  bool has_elf = sdcard_fexists(elf_path);

  if (!has_lua && !has_elf)
    return;

  // Native wins when both exist (rare, but log it)
  if (has_lua && has_elf)
    printf("[LAUNCHER] '%s': both main.lua and main.elf found — using native\n",
           entry->name);

  app_entry_t *app = &s_apps[s_app_count];
  snprintf(app->path, sizeof(app->path), "/apps/%s", entry->name);
  app->type                = has_elf ? APP_TYPE_NATIVE : APP_TYPE_LUA;
  app->has_root_filesystem = false;
  app->has_http            = false;
  app->has_audio           = false;
  app->system_clock_khz    = 0;

  // Try to load app.json for display name / description / id / requirements
  char json_path[160];
  snprintf(json_path, sizeof(json_path), "/apps/%s/app.json", entry->name);
  int json_len = 0;
  char *json = sdcard_read_file(json_path, &json_len);
  if (json) {
    if (!json_get_string(json, "id", app->id, sizeof(app->id)))
      snprintf(app->id, sizeof(app->id), "local.%s", entry->name);
    if (!json_get_string(json, "name", app->name, sizeof(app->name)))
      strncpy(app->name, entry->name, sizeof(app->name));
    if (!json_get_string(json, "description", app->description,
                         sizeof(app->description)))
      app->description[0] = '\0';
    if (!json_get_string(json, "version", app->version, sizeof(app->version)))
      strncpy(app->version, "1.0", sizeof(app->version));

    app->has_root_filesystem = json_has_requirement(json, "root-filesystem");
    app->has_http            = json_has_requirement(json, "http");
    app->has_audio           = json_has_requirement(json, "audio");
    json_get_int(json, "system_clock_khz", &app->system_clock_khz);
    if (!json_get_string(json, "category", app->category, sizeof(app->category)))
      app->category[0] = '\0';

    umm_free(json);
  } else {
    printf("[LAUNCHER] WARNING: failed to read '%s', using dir name\n", json_path);
    snprintf(app->id, sizeof(app->id), "local.%s", entry->name);
    strncpy(app->name, entry->name, sizeof(app->name));
    app->description[0] = '\0';
    strncpy(app->version, "?", sizeof(app->version));
    app->category[0] = '\0';
  }

  // Try to load app icon (PNG first, then BMP)
  app->icon = NULL;
  char icon_path[160];
  snprintf(icon_path, sizeof(icon_path), "/apps/%s/icon.png", entry->name);
  app->icon = image_load(icon_path);
  if (!app->icon) {
    snprintf(icon_path, sizeof(icon_path), "/apps/%s/icon.bmp", entry->name);
    app->icon = image_load(icon_path);
  }

  s_app_count++;
}

static int compare_app_name(const void *a, const void *b) {
  return strcasecmp(((const app_entry_t *)a)->name,
                    ((const app_entry_t *)b)->name);
}

static void scan_apps(void) {
  // Free previously loaded icons before rescan
  for (int i = 0; i < s_app_count; i++) {
    if (s_apps[i].icon) {
      image_free((pc_image_t *)s_apps[i].icon);
      s_apps[i].icon = NULL;
    }
  }

  s_app_count = 0;
  memset(s_apps, 0, sizeof(app_entry_t) * MAX_APPS);
  printf("[LAUNCHER] Scanning /apps directory...\n");
  fflush(stdout);
  sdcard_list_dir("/apps", on_app_dir, NULL);
  if (s_app_count > 1)
    qsort(s_apps, s_app_count, sizeof(app_entry_t), compare_app_name);
  printf("[LAUNCHER] Found %d apps\n", s_app_count);
  fflush(stdout);
}

// ── Category system ──────────────────────────────────────────────────────────

typedef enum {
  CAT_GAMES = 0,
  CAT_TOOLS,
  CAT_SYSTEM,
  CAT_DEMOS,
  CAT_EMULATORS,
  CAT_NETWORK,
  CAT_COUNT,
  CAT_ALL  // virtual: unfiltered
} category_t;

static const char *s_cat_names[CAT_COUNT] = {
    "Games", "Tools", "System", "Demos", "Emulators", "Network"};

static const uint16_t s_cat_colors[CAT_COUNT] = {
    RGB565(255, 100, 50),   // Games: orange
    RGB565(100, 180, 255),  // Tools: blue
    RGB565(160, 160, 160),  // System: gray
    RGB565(255, 200, 50),   // Demos: yellow
    RGB565(150, 100, 255),  // Emulators: purple
    RGB565(50, 200, 150),   // Network: teal
};

// Per-category app indices into s_apps[]
static int s_cat_indices[CAT_COUNT][MAX_APPS];
static int s_cat_counts[CAT_COUNT];

static category_t parse_category(const char *cat_str) {
  if (!cat_str || !cat_str[0]) return CAT_DEMOS;  // default
  if (strcasecmp(cat_str, "games") == 0) return CAT_GAMES;
  if (strcasecmp(cat_str, "tools") == 0) return CAT_TOOLS;
  if (strcasecmp(cat_str, "system") == 0) return CAT_SYSTEM;
  if (strcasecmp(cat_str, "demos") == 0) return CAT_DEMOS;
  if (strcasecmp(cat_str, "emulators") == 0) return CAT_EMULATORS;
  if (strcasecmp(cat_str, "network") == 0) return CAT_NETWORK;
  return CAT_DEMOS;
}

static void build_category_indices(void) {
  memset(s_cat_counts, 0, sizeof(s_cat_counts));
  for (int i = 0; i < s_app_count; i++) {
    category_t cat = parse_category(s_apps[i].category);
    if (cat < CAT_COUNT)
      s_cat_indices[cat][s_cat_counts[cat]++] = i;
  }
}

// ── Launcher rendering ──────────────────────────────────────────────────────

#define ITEM_H 28
#define LIST_X 8
#define LIST_Y 32
#define LIST_VISIBLE 9
#define DESC_SCROLL_RESET_PAUSE 40

#define GRID_X      8
#define GRID_Y      34
#define TILE_W      96
#define TILE_H      88
#define TILE_GAP    8
#define GRID_COLS   3
#define GRID_ROWS   2
#define ALL_APPS_Y  220
#define ICON_SIZE   20  // app icon size in list view

typedef enum {
  VIEW_HOME,      // category grid
  VIEW_CATEGORY,  // filtered app list
  VIEW_ALL,       // all apps list
} launcher_view_t;

static launcher_view_t s_view = VIEW_HOME;
static category_t s_current_cat = CAT_GAMES;
static int s_home_sel = 0;  // 0..5 = tiles, 6 = "All Apps"

static int s_selected = 0;
static int s_scroll = 0;
static int s_desc_scroll = 0;
static int s_desc_scroll_timer = 0;

void launcher_refresh_apps(void) {
  scan_apps();
  build_category_indices();
  s_selected = 0;
  s_scroll = 0;
}

#define C_BG COLOR_BLACK
#define C_HEADER_BG RGB565(20, 20, 60)
#define C_SEL_BG RGB565(40, 80, 160)
#define C_TEXT COLOR_WHITE
#define C_TEXT_DIM COLOR_GRAY
#define C_BATTERY_OK COLOR_GREEN
#define C_BATTERY_LO COLOR_RED
#define C_BORDER RGB565(60, 60, 100)
#define C_TILE_BG RGB565(25, 25, 40)
#define C_TILE_SEL RGB565(40, 60, 120)

// ── Procedural category icons (32x32, drawn with display primitives) ────────

static void draw_cat_icon_games(int cx, int cy, uint16_t color) {
  // Gamepad: body rect + two circle buttons + dpad
  display_fill_rect(cx - 14, cy - 6, 28, 12, color);
  display_fill_circle(cx - 7, cy, 3, COLOR_WHITE);
  display_fill_circle(cx + 7, cy, 3, COLOR_WHITE);
  // Dpad
  display_fill_rect(cx - 2, cy - 5, 4, 10, COLOR_WHITE);
  display_fill_rect(cx - 5, cy - 2, 10, 4, COLOR_WHITE);
}

static void draw_cat_icon_tools(int cx, int cy, uint16_t color) {
  // Wrench shape
  display_fill_circle(cx - 6, cy - 6, 5, color);
  display_fill_circle(cx - 6, cy - 6, 2, C_TILE_BG);
  display_draw_line(cx - 3, cy - 3, cx + 8, cy + 8, color);
  display_draw_line(cx - 2, cy - 3, cx + 9, cy + 8, color);
  display_fill_rect(cx + 5, cy + 5, 6, 6, color);
}

static void draw_cat_icon_system(int cx, int cy, uint16_t color) {
  // Gear: circle with notches
  display_fill_circle(cx, cy, 8, color);
  display_fill_circle(cx, cy, 4, C_TILE_BG);
  // Notches (small rects at cardinal + diagonal points)
  for (int a = 0; a < 8; a++) {
    int dx = (a == 0 || a == 4) ? 0 : ((a < 4) ? 1 : -1);
    int dy = (a == 2 || a == 6) ? 0 : ((a < 2 || a > 6) ? -1 : 1);
    display_fill_rect(cx + dx * 9 - 2, cy + dy * 9 - 2, 4, 4, color);
  }
}

static void draw_cat_icon_demos(int cx, int cy, uint16_t color) {
  // Star / diamond shape
  display_draw_line(cx, cy - 10, cx + 7, cy, color);
  display_draw_line(cx + 7, cy, cx, cy + 10, color);
  display_draw_line(cx, cy + 10, cx - 7, cy, color);
  display_draw_line(cx - 7, cy, cx, cy - 10, color);
  display_fill_circle(cx, cy, 4, color);
}

static void draw_cat_icon_emulators(int cx, int cy, uint16_t color) {
  // Retro screen with antenna
  display_fill_rect(cx - 10, cy - 6, 20, 14, color);
  display_fill_rect(cx - 8, cy - 4, 16, 10, C_TILE_BG);
  // Antenna
  display_draw_line(cx - 4, cy - 6, cx - 8, cy - 12, color);
  display_draw_line(cx + 4, cy - 6, cx + 8, cy - 12, color);
}

static void draw_cat_icon_network(int cx, int cy, uint16_t color) {
  // Globe: circle with cross lines
  display_draw_circle(cx, cy, 9, color);
  display_draw_circle(cx, cy, 10, color);
  display_draw_line(cx - 10, cy, cx + 10, cy, color);
  display_draw_line(cx, cy - 10, cx, cy + 10, color);
  // Latitude arcs (simplified as ellipse-ish lines)
  display_draw_line(cx - 5, cy - 8, cx + 5, cy - 8, color);
  display_draw_line(cx - 5, cy + 8, cx + 5, cy + 8, color);
}

static void draw_category_icon(int cx, int cy, category_t cat) {
  uint16_t color = (cat < CAT_COUNT) ? s_cat_colors[cat] : COLOR_GRAY;
  switch (cat) {
    case CAT_GAMES:    draw_cat_icon_games(cx, cy, color); break;
    case CAT_TOOLS:    draw_cat_icon_tools(cx, cy, color); break;
    case CAT_SYSTEM:   draw_cat_icon_system(cx, cy, color); break;
    case CAT_DEMOS:    draw_cat_icon_demos(cx, cy, color); break;
    case CAT_EMULATORS:draw_cat_icon_emulators(cx, cy, color); break;
    case CAT_NETWORK:  draw_cat_icon_network(cx, cy, color); break;
    default: break;
  }
}

// ── Fallback app icon (colored square with first letter) ────────────────────

static void draw_fallback_icon(int x, int y, int size, const char *name,
                               const char *category) {
  category_t cat = parse_category(category);
  uint16_t color = (cat < CAT_COUNT) ? s_cat_colors[cat] : COLOR_GRAY;
  display_fill_rect(x, y, size, size, color);
  if (name && name[0]) {
    char letter[2] = {name[0], '\0'};
    int tx = x + (size - 6) / 2;
    int ty = y + (size - 8) / 2;
    display_draw_text(tx, ty, letter, COLOR_WHITE, color);
  }
}

// ── Draw app icon (image or fallback) ───────────────────────────────────────

static void draw_app_icon(int x, int y, int size, const app_entry_t *app) {
  if (app->icon) {
    image_draw_scaled((pc_image_t *)app->icon, x, y, size, size);
  } else {
    draw_fallback_icon(x, y, size, app->name, app->category);
  }
}

// ── Home view (category grid) ───────────────────────────────────────────────

static void draw_home_view(void) {
  display_clear(C_BG);
  ui_draw_header("PicOS");

  if (s_app_count == 0) {
    display_draw_text(8, GRID_Y + 30, "No apps found.", C_TEXT_DIM, C_BG);
    display_draw_text(8, GRID_Y + 44, "Copy apps to /apps/ on SD.",
                      C_TEXT_DIM, C_BG);
    ui_draw_footer("", NULL);
    display_flush();
    return;
  }

  // Draw 3x2 grid of category tiles
  for (int r = 0; r < GRID_ROWS; r++) {
    for (int c = 0; c < GRID_COLS; c++) {
      int cat = r * GRID_COLS + c;
      int tx = GRID_X + c * (TILE_W + TILE_GAP);
      int ty = GRID_Y + r * (TILE_H + TILE_GAP);
      bool sel = (s_home_sel == cat);
      int count = s_cat_counts[cat];

      // Tile background
      uint16_t bg = sel ? C_TILE_SEL : C_TILE_BG;
      display_fill_rect(tx, ty, TILE_W, TILE_H, bg);

      // Selection border
      if (sel) {
        display_draw_rect(tx, ty, TILE_W, TILE_H, s_cat_colors[cat]);
        display_draw_rect(tx + 1, ty + 1, TILE_W - 2, TILE_H - 2,
                          s_cat_colors[cat]);
      }

      // Category icon (centered in top portion)
      int icon_cx = tx + TILE_W / 2;
      int icon_cy = ty + 28;
      draw_category_icon(icon_cx, icon_cy, (category_t)cat);

      // Category name (centered)
      const char *name = s_cat_names[cat];
      int name_w = display_text_width(name);
      display_draw_text(tx + (TILE_W - name_w) / 2, ty + 52, name, C_TEXT, bg);

      // App count
      char count_str[16];
      snprintf(count_str, sizeof(count_str), "(%d)", count);
      int count_w = display_text_width(count_str);
      uint16_t count_color = count > 0 ? C_TEXT_DIM : RGB565(80, 80, 80);
      display_draw_text(tx + (TILE_W - count_w) / 2, ty + 66, count_str,
                        count_color, bg);
    }
  }

  // "All Apps" bar
  bool all_sel = (s_home_sel == CAT_COUNT);
  uint16_t all_bg = all_sel ? C_SEL_BG : C_TILE_BG;
  display_fill_rect(GRID_X, ALL_APPS_Y, FB_WIDTH - GRID_X * 2, ITEM_H, all_bg);
  if (all_sel) {
    display_draw_rect(GRID_X, ALL_APPS_Y, FB_WIDTH - GRID_X * 2, ITEM_H,
                      RGB565(100, 140, 255));
  }
  char all_label[32];
  snprintf(all_label, sizeof(all_label), "All Apps (%d)", s_app_count);
  int all_w = display_text_width(all_label);
  display_draw_text((FB_WIDTH - all_w) / 2, ALL_APPS_Y + 10, all_label, C_TEXT,
                    all_bg);

  ui_draw_footer("Arrows:Select  Enter:Open  F10:Menu", NULL);
  display_flush();
}

// ── App list view (filtered or all) ─────────────────────────────────────────

static int list_count(void) {
  if (s_view == VIEW_ALL) return s_app_count;
  if (s_current_cat < CAT_COUNT) return s_cat_counts[s_current_cat];
  return 0;
}

static int list_app_idx(int list_idx) {
  if (s_view == VIEW_ALL) return list_idx;
  if (s_current_cat < CAT_COUNT && list_idx < s_cat_counts[s_current_cat])
    return s_cat_indices[s_current_cat][list_idx];
  return 0;
}

static void draw_app_list(void) {
  display_clear(C_BG);

  // Header with category name
  if (s_view == VIEW_CATEGORY && s_current_cat < CAT_COUNT)
    ui_draw_header(s_cat_names[s_current_cat]);
  else
    ui_draw_header("All Apps");

  int count = list_count();

  if (count == 0) {
    display_draw_text(8, LIST_Y + 20, "No apps in this category.",
                      C_TEXT_DIM, C_BG);
    ui_draw_footer("ESC:Back", NULL);
    display_flush();
    return;
  }

  int text_x = LIST_X + ICON_SIZE + 4;  // shifted right for icon

  for (int i = 0; i < LIST_VISIBLE && (i + s_scroll) < count; i++) {
    int list_idx = i + s_scroll;
    int app_idx = list_app_idx(list_idx);
    int y = LIST_Y + i * ITEM_H;
    bool sel = (list_idx == s_selected);

    uint16_t bg = sel ? C_SEL_BG : C_BG;
    display_fill_rect(LIST_X - 4, y, FB_WIDTH - LIST_X * 2 + 8, ITEM_H - 2, bg);

    // App icon
    draw_app_icon(LIST_X, y + 4, ICON_SIZE, &s_apps[app_idx]);

    // App name
    display_draw_text(text_x, y + 4, s_apps[app_idx].name, C_TEXT, bg);

    // Version (right-aligned)
    if (s_apps[app_idx].version[0]) {
      int vw = display_text_width(s_apps[app_idx].version);
      display_draw_text(FB_WIDTH - 8 - vw, y + 4, s_apps[app_idx].version,
                        C_TEXT_DIM, bg);
    }

    // Description
    if (s_apps[app_idx].description[0]) {
      int max_w = FB_WIDTH - text_x - 8;
      int tw = display_text_width(s_apps[app_idx].description);
      if (tw > max_w && sel) {
        const char *p = s_apps[app_idx].description + s_desc_scroll;
        int avail = strlen(p);
        char buf[64];
        int out_len = (max_w / 6 + 1);
        if (out_len > avail) out_len = avail;
        if (out_len > 63) out_len = 63;
        strncpy(buf, p, out_len);
        buf[out_len] = '\0';
        display_draw_text(text_x, y + 15, buf, C_TEXT_DIM, bg);
      } else {
        display_draw_text(text_x, y + 15, s_apps[app_idx].description,
                          C_TEXT_DIM, bg);
      }
    }
  }

  // Scrollbar
  if (count > LIST_VISIBLE) {
    int bar_h = (LIST_VISIBLE * ITEM_H) * LIST_VISIBLE / count;
    if (bar_h < 4) bar_h = 4;
    int bar_y = LIST_Y + (LIST_VISIBLE * ITEM_H) * s_scroll / count;
    display_fill_rect(FB_WIDTH - 6, LIST_Y, 4, LIST_VISIBLE * ITEM_H, C_BORDER);
    display_fill_rect(FB_WIDTH - 6, bar_y, 4, bar_h, C_TEXT);
  }

  ui_draw_footer("ESC:Back  Enter:Launch  F10:Menu", NULL);
  display_flush();
}

// ── Unified draw dispatch ───────────────────────────────────────────────────

static void draw_current_view(void) {
  switch (s_view) {
    case VIEW_HOME:     draw_home_view(); break;
    case VIEW_CATEGORY: draw_app_list();  break;
    case VIEW_ALL:      draw_app_list();  break;
  }
}

// ── Runner dispatch table ─────────────────────────────────────────────────────

static const AppRunner *s_runners[] = {
    &g_lua_runner,
    &g_native_runner,
    NULL,
};

// ── App launcher
// ──────────────────────────────────────────────────────────────

extern _Atomic bool g_core1_pause;
extern _Atomic bool g_core1_paused;

void launcher_apply_clock(uint32_t khz) {
  if (khz == 0) khz = 200000; // Default OS clock
  uint32_t current_khz = clock_get_hz(clk_sys) / 1000;
  if (khz == current_khz) return;

  printf("[LAUNCHER] Changing clock: %lu -> %lu MHz\n", 
         (unsigned long)(current_khz / 1000), (unsigned long)(khz / 1000));

  // 1. Pause Core 1 background tasks (WiFi/Audio) to avoid bus corruption
  g_core1_pause = true;
  for (int i = 0; i < 200 && !g_core1_paused; i++)
    sleep_ms(1);
  if (!g_core1_paused)
    printf("[LAUNCHER] Core 1 pause timeout (200ms) during clock change\n");

  // 2. Up-clocking: Raise voltage BEFORE increasing frequency
  if (khz > current_khz) {
    enum vreg_voltage v = VREG_VOLTAGE_DEFAULT;  // 1.10V, good to ~200 MHz
    if (khz >= 400000)
      v = VREG_VOLTAGE_1_25;
    else if (khz >= 300000)
      v = VREG_VOLTAGE_1_20;
    else if (khz >= 250000)
      v = VREG_VOLTAGE_1_15;

    if (v != VREG_VOLTAGE_DEFAULT) {
      vreg_set_voltage(v);
      sleep_ms(2);
    }
  }

  // 3. Ensure display DMA is finished before changing clock source
  display_apply_clock(); // This now waits for DMA internally

  // 4. Apply the new system clock
  bool ok = set_sys_clock_khz(khz, false);

  if (!ok) {
    printf("[LAUNCHER] Clock change to %lu MHz failed (PLL cannot produce this frequency)\n",
           (unsigned long)(khz / 1000));
    g_core1_pause = false;
    return;
  }

  // 5. Re-configure peripheral clock so SPI/I2C/UART/PWM stay stable.
  clock_configure(
      clk_peri,
      0,
      CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
      khz * 1000,
      khz * 1000);

  // 6. Update display PIO divider for new clk_sys frequency
  display_apply_clock();

  // 7. Update keyboard I2C divider for new clk_peri frequency
  kbd_apply_clock();

  // 7b. Re-set SD SPI baud rate (derived from clk_peri)
  sdcard_apply_clock();

  // 8. Re-init UART baud rate (depends on clk_peri)
#if LIB_PICO_STDIO_UART
  uart_init(uart0, 115200);
#endif

  // 9. Down-clocking: Lower voltage AFTER decreasing frequency
  if (khz < current_khz) {
    enum vreg_voltage v = VREG_VOLTAGE_DEFAULT;
    if (khz >= 400000)
      v = VREG_VOLTAGE_1_25;
    else if (khz >= 300000)
      v = VREG_VOLTAGE_1_20;
    else if (khz >= 250000)
      v = VREG_VOLTAGE_1_15;

    vreg_set_voltage(v);
    sleep_ms(2);
  }

  // 10. Resume Core 1
  g_core1_pause = false;
}

static volatile const char *s_running_app_name = NULL;
static volatile uint32_t s_app_launch_time_ms = 0;

static bool run_app(int idx) {
  if (idx < 0 || idx >= s_app_count)
    return false;

  app_entry_t *app = &s_apps[idx];
  s_running_app_name = app->name;
  s_app_launch_time_ms = to_ms_since_boot(get_absolute_time());

  // Free any PSRAM used by the MP3 player (from a previous Lua app)
  // so we have maximum memory for the next app.
  mp3_player_deinit();

  printf("[LAUNCHER] Starting app %d '%s' (type=%s), PSRAM free: %zu\n",
         idx, app->name,
         app->type == APP_TYPE_NATIVE ? "native" : "lua",
         lua_psram_alloc_free_size());

  // ── Shared pre-launch setup ───────────────────────────────────────────────

  // Disconnect WiFi before clock change if app doesn't need it.
  // The CYW43 PIO SPI clock divider is set at init time (200 MHz) and is NOT
  // updated by launcher_apply_clock(), so running WiFi at a different sys clock
  // causes SPI timing failures ("hdr mismatch" errors) that stall Core 1.
  if (app->system_clock_khz > 0 && !app->has_http && wifi_is_available()) {
    wifi_status_t wst = wifi_get_status();
    if (wst == WIFI_STATUS_CONNECTED || wst == WIFI_STATUS_CONNECTING ||
        wst == WIFI_STATUS_ONLINE) {
      wifi_disconnect();
      // Wait for Core 1 to process the disconnect request before pausing it
      // for the clock change. wifi_disconnect() queues via IPC ring buffer.
      sleep_ms(50);
    }
  }

  if (app->system_clock_khz > 0) {
    launcher_apply_clock(app->system_clock_khz);
  }

  wifi_set_http_required(app->has_http);

  if (app->has_http && wifi_is_available()) {
    wifi_status_t wst2 = wifi_get_status();
    if (wst2 != WIFI_STATUS_CONNECTED && wst2 != WIFI_STATUS_ONLINE) {
      const char *ssid = config_get("wifi_ssid");
      const char *pass = config_get("wifi_pass");
      if (ssid && ssid[0])
        wifi_connect(ssid, pass ? pass : "");
    }
  }

  // ── Dispatch to runner ────────────────────────────────────────────────────
  bool ok = false;
  for (int i = 0; s_runners[i]; i++) {
    if (s_runners[i]->can_handle(app)) {
      ok = s_runners[i]->run(app);
      break;
    }
  }

  // ── Shared post-exit cleanup ──────────────────────────────────────────────
  if (app->system_clock_khz > 0) {
    launcher_apply_clock(200000); // Reset to system default
  }

  system_menu_clear_items();

  s_running_app_name = NULL;
  s_app_launch_time_ms = 0;

  printf("[LAUNCHER] App '%s' exited (ok=%d), PSRAM free: %zu\n",
         app->name, ok, lua_psram_alloc_free_size());

  return ok;
}

// ── Public interface
// ──────────────────────────────────────────────────────────

// ── Input: Home view grid navigation ────────────────────────────────────────

static bool handle_home_input(uint32_t pressed, bool *dirty) {
  if (pressed & BTN_UP) {
    if (s_home_sel == CAT_COUNT) {
      // "All Apps" → bottom row
      s_home_sel = GRID_COLS + (s_home_sel % GRID_COLS);
      if (s_home_sel >= CAT_COUNT) s_home_sel = CAT_COUNT - 1;
    } else if (s_home_sel >= GRID_COLS) {
      s_home_sel -= GRID_COLS;  // move up one row
    } else {
      s_home_sel = CAT_COUNT;   // wrap: top row → "All Apps"
    }
    *dirty = true;
  }
  if (pressed & BTN_DOWN) {
    if (s_home_sel == CAT_COUNT) {
      s_home_sel = 0;   // wrap: "All Apps" → top row
    } else if (s_home_sel < GRID_COLS) {
      s_home_sel += GRID_COLS;  // move to bottom row
      if (s_home_sel >= CAT_COUNT) s_home_sel = CAT_COUNT - 1;
    } else {
      s_home_sel = CAT_COUNT;   // bottom row → "All Apps"
    }
    *dirty = true;
  }
  if (pressed & BTN_LEFT) {
    if (s_home_sel < CAT_COUNT && (s_home_sel % GRID_COLS) > 0) {
      s_home_sel--;
      *dirty = true;
    }
  }
  if (pressed & BTN_RIGHT) {
    if (s_home_sel < CAT_COUNT && (s_home_sel % GRID_COLS) < GRID_COLS - 1
        && s_home_sel + 1 < CAT_COUNT) {
      s_home_sel++;
      *dirty = true;
    }
  }
  if (pressed & BTN_ENTER) {
    if (s_home_sel == CAT_COUNT) {
      // "All Apps"
      s_view = VIEW_ALL;
      s_selected = 0;
      s_scroll = 0;
      s_desc_scroll = 0;
      s_desc_scroll_timer = 0;
    } else if (s_home_sel < CAT_COUNT) {
      // Enter a category (skip if empty)
      if (s_cat_counts[s_home_sel] > 0) {
        s_view = VIEW_CATEGORY;
        s_current_cat = (category_t)s_home_sel;
        s_selected = 0;
        s_scroll = 0;
        s_desc_scroll = 0;
        s_desc_scroll_timer = 0;
      }
    }
    *dirty = true;
  }
  return false;
}

// ── Input: App list navigation ──────────────────────────────────────────────

static bool handle_list_input(uint32_t pressed, bool *dirty) {
  int count = list_count();

  if (pressed & BTN_ESC) {
    s_view = VIEW_HOME;
    *dirty = true;
    return false;
  }

  if (pressed & BTN_UP) {
    if (count > 0) {
      if (s_selected > 0) s_selected--;
      else s_selected = count - 1;
      if (s_selected < s_scroll) s_scroll = s_selected;
      if (s_selected >= s_scroll + LIST_VISIBLE)
        s_scroll = s_selected - LIST_VISIBLE + 1;
      s_desc_scroll = 0;
      s_desc_scroll_timer = 0;
      *dirty = true;
    }
  }
  if (pressed & BTN_DOWN) {
    if (count > 0) {
      if (s_selected < count - 1) s_selected++;
      else s_selected = 0;
      if (s_selected >= s_scroll + LIST_VISIBLE)
        s_scroll = s_selected - LIST_VISIBLE + 1;
      if (s_selected < s_scroll) s_scroll = s_selected;
      s_desc_scroll = 0;
      s_desc_scroll_timer = 0;
      *dirty = true;
    }
  }
  if (pressed & BTN_LEFT) {
    if (count > 0) {
      int new_sel = s_selected - LIST_VISIBLE;
      if (new_sel < 0) new_sel = 0;
      if (new_sel != s_selected) {
        s_selected = new_sel;
        if (s_selected < s_scroll) s_scroll = s_selected;
        s_desc_scroll = 0;
        s_desc_scroll_timer = 0;
        *dirty = true;
      }
    }
  }
  if (pressed & BTN_RIGHT) {
    if (count > 0) {
      int new_sel = s_selected + LIST_VISIBLE;
      if (new_sel >= count) new_sel = count - 1;
      if (new_sel != s_selected) {
        s_selected = new_sel;
        if (s_selected >= s_scroll + LIST_VISIBLE)
          s_scroll = s_selected - LIST_VISIBLE + 1;
        s_desc_scroll = 0;
        s_desc_scroll_timer = 0;
        *dirty = true;
      }
    }
  }
  if (pressed & BTN_ENTER) {
    if (count > 0 && s_selected < count) {
      int app_idx = list_app_idx(s_selected);
      size_t free_mem = lua_psram_alloc_free_size();
      printf("[LAUNCHER] PSRAM free before launch: %zu bytes\n", free_mem);

      // Preserve view state across app launch
      launcher_view_t saved_view = s_view;
      category_t saved_cat = s_current_cat;

      run_app(app_idx);
      kbd_clear_state();
      scan_apps();
      build_category_indices();

      // Restore view state
      s_view = saved_view;
      s_current_cat = saved_cat;
      s_desc_scroll = 0;
      s_desc_scroll_timer = 0;

      // Clamp selection if app count changed
      int new_count = list_count();
      if (s_selected >= new_count) s_selected = new_count > 0 ? new_count - 1 : 0;
      if (s_scroll > s_selected) s_scroll = s_selected;

      *dirty = true;
    }
  }
  return false;
}

// ── Description scroll update (for list views) ─────────────────────────────

static void update_desc_scroll(bool *dirty) {
  if (s_view == VIEW_HOME) return;

  int count = list_count();
  s_desc_scroll_timer++;
  if (s_desc_scroll_timer >= 10) {
    s_desc_scroll_timer = 0;
    if (s_selected < count) {
      int app_idx = list_app_idx(s_selected);
      if (s_apps[app_idx].description[0]) {
        int text_x = LIST_X + ICON_SIZE + 4;
        int max_w = FB_WIDTH - text_x - 8;
        int tw = display_text_width(s_apps[app_idx].description);
        int desc_len = strlen(s_apps[app_idx].description);
        int max_scroll = desc_len - (max_w / 6);
        if (tw > max_w) {
          if (s_desc_scroll < max_scroll) {
            s_desc_scroll++;
            *dirty = true;
          } else {
            s_desc_scroll = 0;
            s_desc_scroll_timer = -DESC_SCROLL_RESET_PAUSE;
            *dirty = true;
          }
        }
      }
    }
  }
}

// ── Main entry point ────────────────────────────────────────────────────────

void launcher_run(void) {
  if (!s_apps) {
    s_apps = umm_malloc(sizeof(app_entry_t) * MAX_APPS);
    if (!s_apps) {
      printf("[LAUNCHER] FATAL: failed to alloc s_apps in PSRAM\n");
      return;
    }
    memset(s_apps, 0, sizeof(app_entry_t) * MAX_APPS);
  }
  scan_apps();
  build_category_indices();

  // Check for simulator auto-launch
#ifdef PICOS_SIMULATOR
  extern const char* simulator_get_auto_launch_app(void);
  const char* auto_launch = simulator_get_auto_launch_app();
  if (auto_launch) {
    printf("[LAUNCHER] Auto-launching: %s\n", auto_launch);
    fflush(stdout);
    if (launcher_launch_by_name(auto_launch)) {
      printf("[LAUNCHER] Auto-launched app exited, returning to launcher\n");
      fflush(stdout);
    } else {
      printf("[LAUNCHER] Failed to auto-launch: %s\n", auto_launch);
      fflush(stdout);
    }
  }
#else
  (void)0;
#endif

  draw_current_view();

  while (true) {
    kbd_poll();
    watchdog_update();

    dev_commands_poll();
    dev_commands_process();

#ifdef PICOS_SIMULATOR
    extern bool sim_handler_check_launch(void);
    bool sim_launched = sim_handler_check_launch();
#else
    bool sim_launched = false;
#endif

    bool dirty = sim_launched;
    if (dev_commands_get_pending_launch()) {
      dev_commands_clear_exit();
      if (launcher_launch_by_name(dev_commands_get_pending_launch())) {
        kbd_clear_state();
        scan_apps();
        build_category_indices();
        dirty = true;
      }
      dev_commands_clear_pending_launch();
    }

#ifdef PICOS_SIMULATOR
    {
      extern volatile int g_running;
      if (!g_running) dev_commands_set_exit();
    }
#endif
    if (dev_commands_wants_exit()) {
      printf("[LAUNCHER] Exit requested, shutting down...\n");
      fflush(stdout);
      dev_commands_clear_exit();
      break;
    }

    if (dev_commands_wants_list()) {
      launcher_list_apps();
      dev_commands_clear_list();
    }
    if (dev_commands_wants_reboot()) {
      printf("[DEV] Rebooting...\n");
      stdio_flush();
      sleep_ms(100);
      watchdog_reboot(0, 0, 0);
    }
    if (dev_commands_wants_reboot_flash()) {
      printf("[DEV] Rebooting to BOOTSEL mode...\n");
      stdio_flush();
      sleep_ms(100);
      reset_usb_boot(0, 0);
    }

    if (kbd_consume_menu_press()) {
      system_menu_show(NULL);
      dirty = true;
    }
    if (kbd_consume_screenshot_press())
      screenshot_save();
    if (screenshot_check_scheduled())
      screenshot_save();

    uint32_t pressed = kbd_get_buttons_pressed();

    // View-specific input handling
    if (pressed) {
      if (s_view == VIEW_HOME)
        handle_home_input(pressed, &dirty);
      else
        handle_list_input(pressed, &dirty);
    }

    if (ui_needs_header_redraw())
      dirty = true;

    // Description scrolling (list views only)
    update_desc_scroll(&dirty);

    if (dirty)
      draw_current_view();

    sleep_ms(16); // ~60 Hz polling
  }
}

// ── Dev command support ─────────────────────────────────────────────────────

void launcher_list_apps(void) {
  printf("[DEV] Available apps:\n");
  for (int i = 0; i < s_app_count; i++) {
    printf("  %s  (%s)\n", s_apps[i].name, s_apps[i].id);
  }
  printf("[DEV] Total: %d apps\n", s_app_count);
}

bool launcher_launch_by_id(const char *id) {
  if (!id || !id[0]) {
    return false;
  }

  // Find the app by ID
  for (int i = 0; i < s_app_count; i++) {
    if (strcmp(s_apps[i].id, id) == 0) {
      printf("[DEV] Launching app by ID: %s (%s)\n", id, s_apps[i].name);
      stdio_flush();
      run_app(i);
      return true;
    }
  }

  return false;
}

bool launcher_launch_by_name(const char *name) {
  if (!name || !name[0]) {
    printf("[DEV] Error: no app name provided\n");
    return false;
  }

  // First try to match by ID
  if (launcher_launch_by_id(name)) {
    return true;
  }

  // Fall back to display name match (case-insensitive)
  for (int i = 0; i < s_app_count; i++) {
    if (strcasecmp(s_apps[i].name, name) == 0) {
      printf("[DEV] Launching app: %s\n", name);
      stdio_flush();
      run_app(i);
      return true;
    }
  }

  // Fall back to directory name match (e.g. "doom" matches "/apps/doom")
  for (int i = 0; i < s_app_count; i++) {
    const char *slash = strrchr(s_apps[i].path, '/');
    const char *dirname = slash ? slash + 1 : s_apps[i].path;
    if (strcasecmp(dirname, name) == 0) {
      printf("[DEV] Launching app by dir: %s (%s)\n", name, s_apps[i].name);
      stdio_flush();
      run_app(i);
      return true;
    }
  }

  printf("[DEV] Error: app '%s' not found\n", name);
  return false;
}

const char* launcher_get_running_app_name(void) {
  return (const char*)s_running_app_name;
}

uint32_t launcher_get_app_uptime_ms(void) {
  uint32_t t = s_app_launch_time_ms;
  if (t == 0) return 0;
  return to_ms_since_boot(get_absolute_time()) - t;
}
