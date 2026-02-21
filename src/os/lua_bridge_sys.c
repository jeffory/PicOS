#include "lua_bridge_internal.h"

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

    // Poll WiFi and fire HTTP callbacks while sleeping so async requests
    // can progress even if the app is just waiting.
    wifi_poll();
    http_lua_fire_pending(L);

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
  // RP2350: VBUS presence is readable via USB hardware; implement if needed.
  // Stub returns false for now.
  lua_pushboolean(L, false);
  return 1;
}

// Exit the current app cleanly, returning to the launcher.
// Equivalent to `return` at the top level of main.lua, but works from
// any call depth. The launcher detects the sentinel and skips the error screen.
static int l_sys_exit(lua_State *L) {
  (void)L;
  return luaL_error(L, "__picocalc_exit__");
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

static int l_sys_clearMenuItems(lua_State *L) {
  for (int i = 0; i < s_lua_callback_count; i++)
    luaL_unref(L, LUA_REGISTRYINDEX, s_lua_callbacks[i].ref);
  s_lua_callback_count = 0;
  system_menu_clear_items();
  return 0;
}

static const luaL_Reg l_sys_lib[] = {{"getTimeMs", l_sys_getTimeMs},
                                     {"getBattery", l_sys_getBattery},
                                     {"log", l_sys_log},
                                     {"sleep", l_sys_sleep},
                                     {"exit", l_sys_exit},
                                     {"reboot", l_sys_reboot},
                                     {"isUSBPowered", l_sys_isUSBPowered},
                                     {"getClock", l_sys_getClock},
                                     {"addMenuItem", l_sys_addMenuItem},
                                     {"clearMenuItems", l_sys_clearMenuItems},
                                     {NULL, NULL}};


void lua_bridge_sys_init(lua_State *L) {
  
  s_lua_callback_count = 0;
  system_menu_clear_items();

register_subtable(L, "sys", l_sys_lib);
}
