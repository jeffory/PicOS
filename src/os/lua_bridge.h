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

// ── App exit ─────────────────────────────────────────────────────────────────
// sys.exit(), the system menu's "Exit App" and the dev `exit` command
// (exit_app) all end in lua_bridge_raise_exit. The request is sticky: it sets
// a flag the Lua runner owns (cleared only when the app's VM has returned),
// raises the exit sentinel (a unique light userdata) as an ordinary Lua
// error, and drops the count hook to every instruction. A pcall/xpcall (or
// coroutine.resume, or a C callback's lua_pcall) that swallows the sentinel
// cannot keep the app alive: the next instruction outside it raises again,
// so the error climbs one protected call per instruction until it reaches
// the runner. Modal loops see the request through dev_commands_wants_exit(),
// which it also sets.
extern char lua_bridge_exit_tag; // address used as sentinel, value irrelevant
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void lua_bridge_raise_exit(lua_State *L);
// True from the first exit request until the runner resets it.
bool lua_bridge_exit_requested(void);
// Runner only, after the app's pcall returned: clears the request (and the
// dev exit flag) and restores the normal count hook, so __gc handlers run
// during lua_close are not interrupted.
void lua_bridge_exit_reset(lua_State *L);
static inline bool lua_bridge_is_exit_sentinel(lua_State *L, int idx) {
  return lua_islightuserdata(L, idx) &&
         lua_touserdata(L, idx) == &lua_bridge_exit_tag;
}

// Run one update tick: poll input, check for menu button, yield to app.
// Returns false if the app requested exit (returned from its update()).
bool lua_bridge_tick(lua_State *L);

// Push a Lua error message as a formatted string to the display.
// Used when pcall() catches a Lua runtime error.
void lua_bridge_show_error(lua_State *L, const char *context);
