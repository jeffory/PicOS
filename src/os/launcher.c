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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── App discovery
// ─────────────────────────────────────────────────────────────

#define MAX_APPS 32

static app_entry_t s_apps[MAX_APPS];
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

static void draw_header(void) { ui_draw_header("PicoCalc OS"); }

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
        const char *p = s_apps[idx].description + s_desc_scroll;
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

static bool run_app(int idx) {
  if (idx < 0 || idx >= s_app_count)
    return false;

  app_entry_t *app = &s_apps[idx];

  printf("[LAUNCHER] Starting app %d '%s' (type=%s), PSRAM free: %zu\n",
         idx, app->name,
         app->type == APP_TYPE_NATIVE ? "native" : "lua",
         lua_psram_alloc_free_size());

  // ── Shared pre-launch setup ───────────────────────────────────────────────
  wifi_set_http_required(app->has_http);

  if (app->has_http && wifi_is_available()) {
    if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
      const char *ssid = config_get("wifi_ssid");
      const char *pass = config_get("wifi_pass");
      if (ssid && ssid[0])
        wifi_connect(ssid, pass ? pass : "");
    }
  }

  if (app->has_audio)
    audio_init();

  // ── Dispatch to runner ────────────────────────────────────────────────────
  bool ok = false;
  for (int i = 0; s_runners[i]; i++) {
    if (s_runners[i]->can_handle(app)) {
      ok = s_runners[i]->run(app);
      break;
    }
  }

  // ── Shared post-exit cleanup ──────────────────────────────────────────────
  system_menu_clear_items();

  printf("[LAUNCHER] App '%s' exited (ok=%d), PSRAM free: %zu\n",
         app->name, ok, lua_psram_alloc_free_size());

  return ok;
}

// ── Public interface
// ──────────────────────────────────────────────────────────

void launcher_run(void) {
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
      if (s_selected > 0) {
        s_selected--;
        if (s_selected < s_scroll)
          s_scroll = s_selected;
        s_desc_scroll = 0;
        s_desc_scroll_timer = 0;
        dirty = true;
      }
    }
    if (pressed & BTN_DOWN) {
      if (s_selected < s_app_count - 1) {
        s_selected++;
        if (s_selected >= s_scroll + LIST_VISIBLE)
          s_scroll = s_selected - LIST_VISIBLE + 1;
        s_desc_scroll = 0;
        s_desc_scroll_timer = 0;
        dirty = true;
      }
    }

    if (pressed & BTN_LEFT) {
      if (s_desc_scroll > 0) {
        s_desc_scroll--;
        dirty = true;
      }
    }
    if (pressed & BTN_RIGHT) {
      s_desc_scroll++;
      dirty = true;
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
      if (s_selected < s_app_count) {
        int tw = display_text_width(s_apps[s_selected].description);
        int max_w = FB_WIDTH - LIST_X * 2 - 4;
        int desc_len = strlen(s_apps[s_selected].description);
        int max_scroll = desc_len - (max_w / 6);
        if (tw > max_w && s_desc_scroll < max_scroll)
          s_desc_scroll++;
      }
    }

    if (dirty)
      draw_launcher();

    sleep_ms(16); // ~60 Hz polling
  }
}
