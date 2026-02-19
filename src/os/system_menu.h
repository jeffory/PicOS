#pragma once
#include <stdbool.h>
#include "lua.h"

// =============================================================================
// System Menu Overlay
//
// An OS-level menu triggered by the F10 key (BTN_F 0x90).
// When invoked, it pauses the running Lua app, draws a translucent overlay
// with built-in items (Brightness, Battery, WiFi, Reboot, Exit App) plus any
// app-registered custom items, and resumes the app on dismiss.
//
// The menu is triggered via a Lua instruction-count hook installed in
// lua_bridge_register(), so no changes to the app run loop are required.
// =============================================================================

#define SYSMENU_MAX_APP_ITEMS 4

// Call once at OS init (before launcher_run).
void system_menu_init(void);

// Register an app item (called via picocalc.sys.addMenuItem in Lua).
// At most SYSMENU_MAX_APP_ITEMS items; extras are silently ignored.
void system_menu_add_item(const char *label,
                          void (*callback)(void *user), void *user);

// Remove all app-registered items.
// Called automatically by launcher.c after each app exits.
void system_menu_clear_items(void);

// Show the menu synchronously. Blocks until the user dismisses or acts.
// May raise a Lua error (via luaL_error) when "Exit App" is selected —
// this propagates to lua_pcall in launcher.c exactly like picocalc.sys.exit().
void system_menu_show(lua_State *L);
