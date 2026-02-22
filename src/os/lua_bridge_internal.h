#pragma once
#include "lua_bridge.h"

// All original lua_bridge.c includes to be shared
#include "../drivers/audio.h"
#include "../drivers/display.h"
#include "../drivers/http.h"
#include "../drivers/keyboard.h"
#include "../drivers/sdcard.h"
#include "../drivers/wifi.h"
#include "../os/clock.h"
#include "../os/config.h"
#include "../os/os.h"
#include "../os/screenshot.h"
#include "../os/system_menu.h"
#include "../os/ui.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/umm_malloc/src/umm_malloc.h"
#include "image_decoders.h"

uint16_t l_checkcolor(lua_State *L, int idx);
bool fs_sandbox_check(lua_State *L, const char *path, bool write);
void http_lua_fire_pending(lua_State *L);
extern bool s_screenshot_pending;

void register_subtable(lua_State *L, const char *name, const luaL_Reg *funcs);

void lua_bridge_display_init(lua_State *L);
void lua_bridge_input_init(lua_State *L);
void lua_bridge_sys_init(lua_State *L);
void lua_bridge_fs_init(lua_State *L);
void lua_bridge_network_init(lua_State *L);
void lua_bridge_config_init(lua_State *L);
void lua_bridge_perf_init(lua_State *L);
void lua_bridge_graphics_init(lua_State *L);
void lua_bridge_ui_init(lua_State *L);
void lua_bridge_audio_init(lua_State *L);
void lua_bridge_sound_init(lua_State *L);
