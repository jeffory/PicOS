#include "launcher.h"
#include "launcher_types.h"
#include "app_runner.h"
#include "lua_runner.h"
#include "native_loader.h"
#include "../drivers/audio.h"
#include "../drivers/display.h"
#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../drivers/wifi.h"

#include "clock.h"
#include "config.h"
#include "lua_psram_alloc.h"
#include "screenshot.h"
#include "system_menu.h"
#include "ui.h"
#include "umm_malloc.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/vreg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  const char *p = strstr(json, search);
  if (!p)
    return false;
  p += strlen(search);
  while (*p == ' ' || *p == ':' || *p == '\t')
    p++;
  if (*p != '"')
    return false;
  p++; // skip opening quote
  int i = 0;
  while (*p && *p != '"' && i < out_len - 1)
    out[i++] = *p++;
  out[i] = '\0';
  return true;
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

    umm_free(json);
  } else {
    snprintf(app->id, sizeof(app->id), "local.%s", entry->name);
    strncpy(app->name, entry->name, sizeof(app->name));
    app->description[0] = '\0';
    strncpy(app->version, "?", sizeof(app->version));
  }

  s_app_count++;
}

static void scan_apps(void) {
  s_app_count = 0;
  sdcard_list_dir("/apps", on_app_dir, NULL);
}

// ── Launcher rendering
// ────────────────────────────────────────────────────────

#define ITEM_H 28
#define LIST_X 8
#define LIST_Y 32
#define LIST_VISIBLE 9
#define DESC_SCROLL_RESET_PAUSE 40  // frames to pause at start before re-scrolling

static int s_selected = 0;
static int s_scroll = 0;
static int s_desc_scroll = 0;
static int s_desc_scroll_timer = 0;

void launcher_refresh_apps(void) {
  scan_apps();
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

static void draw_header(void) { ui_draw_header("PicOS"); }

static void draw_footer(void) {
  ui_draw_footer("Enter:Launch  Esc:Exit app  F10:Menu", NULL);
}

static void draw_launcher(void) {
  display_clear(C_BG);
  draw_header();
  draw_footer();

  if (s_app_count == 0) {
    display_draw_text(8, LIST_Y + 8, "No apps found.", C_TEXT_DIM, C_BG);
    display_draw_text(8, LIST_Y + 20, "Copy apps to /apps/ on SD card.",
                      C_TEXT_DIM, C_BG);
    display_flush();
    return;
  }

  for (int i = 0; i < LIST_VISIBLE && (i + s_scroll) < s_app_count; i++) {
    int idx  = i + s_scroll;
    int y    = LIST_Y + i * ITEM_H;
    bool sel = (idx == s_selected);

    uint16_t bg = sel ? C_SEL_BG : C_BG;
    display_fill_rect(LIST_X - 4, y, FB_WIDTH - LIST_X * 2 + 8, ITEM_H - 2, bg);

    display_draw_text(LIST_X, y + 4, s_apps[idx].name, C_TEXT, bg);
    if (s_apps[idx].description[0]) {
      int max_w = FB_WIDTH - LIST_X * 2 - 4;
      int tw = display_text_width(s_apps[idx].description);
      if (tw > max_w) {
        const char *p = s_apps[idx].description + (sel ? s_desc_scroll : 0);
        int avail = strlen(p);
        char buf[64];
        int out_len = (avail * 6 > max_w * 6) ? (max_w / 6 + 1) : avail;
        if (out_len > 63) out_len = 63;
        strncpy(buf, p, out_len);
        buf[out_len] = '\0';
        display_draw_text(LIST_X, y + 15, buf, C_TEXT_DIM, bg);
      } else {
        display_draw_text(LIST_X, y + 15, s_apps[idx].description, C_TEXT_DIM, bg);
      }
    }
  }

  // Scrollbar
  if (s_app_count > LIST_VISIBLE) {
    int bar_h = (LIST_VISIBLE * ITEM_H) * LIST_VISIBLE / s_app_count;
    int bar_y = LIST_Y + (LIST_VISIBLE * ITEM_H) * s_scroll / s_app_count;
    display_fill_rect(FB_WIDTH - 6, LIST_Y, 4, LIST_VISIBLE * ITEM_H, C_BORDER);
    display_fill_rect(FB_WIDTH - 6, bar_y, 4, bar_h, C_TEXT);
  }

  display_flush();
}

// ── Runner dispatch table ─────────────────────────────────────────────────────

static const AppRunner *s_runners[] = {
    &g_lua_runner,
    &g_native_runner,
    NULL,
};

// ── App launcher
// ──────────────────────────────────────────────────────────────

extern volatile bool g_core1_pause;

static void launcher_apply_clock(uint32_t khz) {
  if (khz == 0) khz = 200000; // Default OS clock
  uint32_t current_khz = clock_get_hz(clk_sys) / 1000;
  if (khz == current_khz) return;

  printf("[LAUNCHER] Changing clock: %lu -> %lu MHz\n", 
         (unsigned long)(current_khz / 1000), (unsigned long)(khz / 1000));

  // 1. Pause Core 1 background tasks (WiFi/Audio) to avoid bus corruption
  g_core1_pause = true;
  sleep_ms(2); // Give Core 1 a moment to notice and hit its sleep_ms(1)

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

static bool run_app(int idx) {
  if (idx < 0 || idx >= s_app_count)
    return false;

  app_entry_t *app = &s_apps[idx];

  printf("[LAUNCHER] Starting app %d '%s' (type=%s), PSRAM free: %zu\n",
         idx, app->name,
         app->type == APP_TYPE_NATIVE ? "native" : "lua",
         lua_psram_alloc_free_size());

  // ── Shared pre-launch setup ───────────────────────────────────────────────
  if (app->system_clock_khz > 0) {
    launcher_apply_clock(app->system_clock_khz);
  }

  wifi_set_http_required(app->has_http);

  if (app->has_http && wifi_is_available()) {
    if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
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

  printf("[LAUNCHER] App '%s' exited (ok=%d), PSRAM free: %zu\n",
         app->name, ok, lua_psram_alloc_free_size());

  return ok;
}

// ── Public interface
// ──────────────────────────────────────────────────────────

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
  draw_launcher();

  while (true) {
    kbd_poll();
    // wifi_poll() is now driven by Core 1 — do not call from Core 0

    bool dirty = false;

    if (kbd_consume_menu_press()) {
      system_menu_show(NULL);
      dirty = true;
    }
    if (kbd_consume_screenshot_press())
      screenshot_save();
    if (screenshot_check_scheduled())
      screenshot_save();

    uint32_t pressed = kbd_get_buttons_pressed();

    if (pressed & BTN_UP) {
      if (s_app_count > 0) {
        if (s_selected > 0)
          s_selected--;
        else
          s_selected = s_app_count - 1;  // wrap to bottom
        if (s_selected < s_scroll)
          s_scroll = s_selected;
        if (s_selected >= s_scroll + LIST_VISIBLE)
          s_scroll = s_selected - LIST_VISIBLE + 1;
        s_desc_scroll = 0;
        s_desc_scroll_timer = 0;
        dirty = true;
      }
    }
    if (pressed & BTN_DOWN) {
      if (s_app_count > 0) {
        if (s_selected < s_app_count - 1)
          s_selected++;
        else
          s_selected = 0;  // wrap to top
        if (s_selected >= s_scroll + LIST_VISIBLE)
          s_scroll = s_selected - LIST_VISIBLE + 1;
        if (s_selected < s_scroll)
          s_scroll = s_selected;
        s_desc_scroll = 0;
        s_desc_scroll_timer = 0;
        dirty = true;
      }
    }

    if (pressed & BTN_LEFT) {
      if (s_app_count > 0) {
        int new_sel = s_selected - LIST_VISIBLE;
        if (new_sel < 0) new_sel = 0;
        if (new_sel != s_selected) {
          s_selected = new_sel;
          if (s_selected < s_scroll)
            s_scroll = s_selected;
          s_desc_scroll = 0;
          s_desc_scroll_timer = 0;
          dirty = true;
        }
      }
    }
    if (pressed & BTN_RIGHT) {
      if (s_app_count > 0) {
        int new_sel = s_selected + LIST_VISIBLE;
        if (new_sel >= s_app_count) new_sel = s_app_count - 1;
        if (new_sel != s_selected) {
          s_selected = new_sel;
          if (s_selected >= s_scroll + LIST_VISIBLE)
            s_scroll = s_selected - LIST_VISIBLE + 1;
          s_desc_scroll = 0;
          s_desc_scroll_timer = 0;
          dirty = true;
        }
      }
    }

    if (pressed & BTN_ENTER) {
      size_t free_mem = lua_psram_alloc_free_size();
      printf("[LAUNCHER] PSRAM free before launch: %zu bytes\n", free_mem);
      run_app(s_selected);
      kbd_clear_state();
      scan_apps();
      s_desc_scroll = 0;
      s_desc_scroll_timer = 0;
      dirty      = true;
    }

    if (ui_needs_header_redraw())
      dirty = true;

    s_desc_scroll_timer++;
    if (s_desc_scroll_timer >= 10) {
      s_desc_scroll_timer = 0;
      if (s_selected < s_app_count && s_apps[s_selected].description[0]) {
        int tw = display_text_width(s_apps[s_selected].description);
        int max_w = FB_WIDTH - LIST_X * 2 - 4;
        int desc_len = strlen(s_apps[s_selected].description);
        int max_scroll = desc_len - (max_w / 6);
        if (tw > max_w) {
          if (s_desc_scroll < max_scroll) {
            s_desc_scroll++;
            dirty = true;
          } else {
            // Reached end — reset and pause before cycling
            s_desc_scroll = 0;
            s_desc_scroll_timer = -DESC_SCROLL_RESET_PAUSE;
            dirty = true;
          }
        }
      }
    }

    if (dirty)
      draw_launcher();

    sleep_ms(16); // ~60 Hz polling
  }
}

// ── Dev command support ─────────────────────────────────────────────────────

void launcher_list_apps(void) {
  printf("[DEV] Available apps:\n");
  for (int i = 0; i < s_app_count; i++) {
    printf("  %s\n", s_apps[i].name);
  }
  printf("[DEV] Total: %d apps\n", s_app_count);
}

bool launcher_launch_by_name(const char *name) {
  if (!name || !name[0]) {
    printf("[DEV] Error: no app name provided\n");
    return false;
  }

  // Find the app by name
  int app_idx = -1;
  for (int i = 0; i < s_app_count; i++) {
    if (strcmp(s_apps[i].name, name) == 0) {
      app_idx = i;
      break;
    }
  }

  if (app_idx < 0) {
    printf("[DEV] Error: app '%s' not found\n", name);
    return false;
  }

  printf("[DEV] Launching app: %s\n", name);
  // TODO: Actually launch the app
  // For now, just return true - actual launching would be done by the caller
  return true;
}

const char* launcher_get_running_app_name(void) {
  // This would need to be tracked - for now return NULL
  return NULL;
}
