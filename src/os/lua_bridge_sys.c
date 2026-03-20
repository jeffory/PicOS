#include "lua_bridge_internal.h"
#include "lua_psram_alloc.h"
#include "ota_update.h"
#include "version.h"
#include "../dev_commands.h"
#include "../hardware.h"
#include "hardware/gpio.h"
#include <malloc.h>

// ── picocalc.sys.* ───────────────────────────────────────────────────────────

static int l_sys_getTimeMs(lua_State *L) {
  lua_pushinteger(L, (lua_Integer)to_ms_since_boot(get_absolute_time()));
  return 1;
}

static int l_sys_getBattery(lua_State *L) {
  // Battery reads are slow I2C round-trips — cache for 5 seconds.
  static int s_cached = -1;
  static uint32_t s_last_ms = 0;
  uint32_t now = (uint32_t)to_ms_since_boot(get_absolute_time());
  if (s_last_ms == 0 || now - s_last_ms >= 5000) {
    s_cached = kbd_get_battery_percent();
    s_last_ms = now;
  }
  lua_pushinteger(L, s_cached);
  return 1;
}

static int l_sys_log(lua_State *L) {
  const char *msg = luaL_checkstring(L, 1);
  printf("[APP] %s\n", msg);
  return 0;
}

static int l_sys_sleep(lua_State *L) {
  int ms = (int)luaL_checkinteger(L, 1);
  // Do NOT call kbd_poll() here — it would drain the STM32 FIFO and consume
  // character/button events that the app expects to read via input.update().
  // The Lua instruction hook (fires every 256 opcodes) handles menu detection
  // immediately after sleep returns.
  uint32_t end_ms =
      (uint32_t)to_ms_since_boot(get_absolute_time()) + (uint32_t)ms;
  while (true) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now >= end_ms)
      break;

    // Fire HTTP/TCP callbacks while sleeping so async requests can progress.
    // WiFi is driven by Core 1; no poll needed here.
    http_lua_fire_pending(L);
    tcp_lua_fire_pending(L);

    // Process dev commands (keypress, etc.) while sleeping.
    dev_commands_poll();
    dev_commands_process();

    uint32_t remaining = end_ms - now;
    sleep_ms(remaining < 10 ? remaining : 10);
  }
  return 0;
}

static int l_sys_reboot(lua_State *L) {
  (void)L;
  watchdog_enable(1, true);
  for (;;)
    tight_loop_contents();
  return 0;
}

static int l_sys_isUSBPowered(lua_State *L) {
  lua_pushboolean(L, gpio_get(USB_VBUS_PIN));
  return 1;
}

static int l_sys_getPowerStatus(lua_State *L) {
  lua_createtable(L, 0, 2);
  lua_pushboolean(L, gpio_get(USB_VBUS_PIN));
  lua_setfield(L, -2, "charging");
  lua_pushinteger(L, kbd_get_battery_percent());
  lua_setfield(L, -2, "percent");
  return 1;
}

// Exit the current app cleanly, returning to the launcher.
// Equivalent to `return` at the top level of main.lua, but works from
// any call depth. The launcher detects the sentinel and skips the error screen.
static int l_sys_exit(lua_State *L) {
  lua_bridge_raise_exit(L);
  return 0; // unreachable
}

// ── picocalc.sys.addMenuItem / clearMenuItems
// ───────────────────────────────── Lua-registered callbacks are stored here as
// Lua registry references. A C trampoline is passed to system_menu_add_item()
// so that calling the menu item invokes the original Lua function.

typedef struct {
  lua_State *L;
  int ref; // LUA_REGISTRYINDEX reference to the Lua function
} lua_callback_t;

static lua_callback_t s_lua_callbacks[SYSMENU_MAX_APP_ITEMS];
static int s_lua_callback_count = 0;

static void lua_menu_trampoline(void *user) {
  lua_callback_t *cb = (lua_callback_t *)user;
  lua_rawgeti(cb->L, LUA_REGISTRYINDEX, cb->ref);
  lua_call(cb->L, 0, 0); // propagates errors (including sys.exit() sentinel)
}

// picocalc.sys.getClock() → {synced=bool, hour=int, min=int, sec=int,
// epoch=int} epoch is UTC Unix seconds; hour/min/sec are UTC + tz_offset.
static int l_sys_getClock(lua_State *L) {
  int h = 0, m = 0, s = 0;
  bool synced = clock_get_time(&h, &m, &s);
  lua_createtable(L, 0, 5);
  lua_pushboolean(L, synced);
  lua_setfield(L, -2, "synced");
  lua_pushinteger(L, h);
  lua_setfield(L, -2, "hour");
  lua_pushinteger(L, m);
  lua_setfield(L, -2, "min");
  lua_pushinteger(L, s);
  lua_setfield(L, -2, "sec");
  lua_pushinteger(L, (lua_Integer)clock_get_epoch());
  lua_setfield(L, -2, "epoch");
  return 1;
}

static int l_sys_getMemInfo(lua_State *L) {
  size_t psram_free  = lua_psram_alloc_free_size();
  size_t psram_total = lua_psram_alloc_total_size();
  size_t psram_used  = psram_total - psram_free;

  struct mallinfo mi = mallinfo();

  lua_createtable(L, 0, 5);
  lua_pushinteger(L, (lua_Integer)psram_free);
  lua_setfield(L, -2, "psram_free");
  lua_pushinteger(L, (lua_Integer)psram_used);
  lua_setfield(L, -2, "psram_used");
  lua_pushinteger(L, (lua_Integer)psram_total);
  lua_setfield(L, -2, "psram_total");
  lua_pushinteger(L, (lua_Integer)mi.fordblks);
  lua_setfield(L, -2, "sram_free");
  lua_pushinteger(L, (lua_Integer)mi.uordblks);
  lua_setfield(L, -2, "sram_used");
  return 1;
}

static int l_sys_addMenuItem(lua_State *L) {
  const char *label = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);

  if (s_lua_callback_count >= SYSMENU_MAX_APP_ITEMS)
    return luaL_error(L, "too many menu items (max %d)", SYSMENU_MAX_APP_ITEMS);

  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_callback_t *cb = &s_lua_callbacks[s_lua_callback_count++];
  cb->L = L;
  cb->ref = ref;

  system_menu_add_item(label, lua_menu_trampoline, cb);
  return 0;
}

static int l_sys_getVersion(lua_State *L) {
  lua_pushstring(L, PICOS_VERSION);
  return 1;
}

static int l_sys_applyUpdate(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  const char *err = NULL;
  bool ok = ota_trigger_update(path, &err);
  if (!ok) {
    lua_pushboolean(L, false);
    lua_pushstring(L, err ? err : "Unknown error");
    return 2;
  }
  // Unreachable — ota_trigger_update reboots on success
  return 0;
}

static int l_sys_clearMenuItems(lua_State *L) {
  for (int i = 0; i < s_lua_callback_count; i++)
    luaL_unref(L, LUA_REGISTRYINDEX, s_lua_callbacks[i].ref);
  s_lua_callback_count = 0;
  system_menu_clear_items();
  return 0;
}

static const luaL_Reg l_sys_lib[] = {{"getMemInfo", l_sys_getMemInfo},
                                     {"getTimeMs", l_sys_getTimeMs},
                                     {"getBattery", l_sys_getBattery},
                                     {"log", l_sys_log},
                                     {"sleep", l_sys_sleep},
                                     {"exit", l_sys_exit},
                                     {"reboot", l_sys_reboot},
                                     {"isUSBPowered", l_sys_isUSBPowered},
                                     {"getPowerStatus", l_sys_getPowerStatus},
                                     {"getClock", l_sys_getClock},
                                     {"getVersion", l_sys_getVersion},
                                     {"applyUpdate", l_sys_applyUpdate},
                                     {"addMenuItem", l_sys_addMenuItem},
                                     {"clearMenuItems", l_sys_clearMenuItems},
                                     {NULL, NULL}};


void lua_bridge_sys_init(lua_State *L) {
  
  s_lua_callback_count = 0;
  system_menu_clear_items();

register_subtable(L, "sys", l_sys_lib);
}
