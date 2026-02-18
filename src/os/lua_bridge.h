#pragma once

#include "lua.h"
#include "../os/os.h"

// =============================================================================
// Lua ↔ OS Bridge
//
// This file declares all the C functions that are registered as Lua modules.
// Together they form the complete Lua-side API that app scripts call.
//
// In Lua, everything is accessed via the `picocalc` global table:
//
//   picocalc.display.clear(0x0000)
//   picocalc.display.drawText(10, 20, "Hello!", 0xFFFF, 0x0000)
//   picocalc.display.flush()
//
//   local btns = picocalc.input.getButtons()
//   if btns & picocalc.input.BTN_ENTER ~= 0 then ... end
//
//   picocalc.sys.log("starting app")
//   local bat = picocalc.sys.getBattery()
//
// =============================================================================

// Register all picocalc.* sub-modules into the Lua state.
// Call this once after lua_newstate(), before running any app code.
void lua_bridge_register(lua_State *L);

// Run one update tick: poll input, check for menu button, yield to app.
// Returns false if the app requested exit (returned from its update()).
bool lua_bridge_tick(lua_State *L);

// Push a Lua error message as a formatted string to the display.
// Used when pcall() catches a Lua runtime error.
void lua_bridge_show_error(lua_State *L, const char *context);
