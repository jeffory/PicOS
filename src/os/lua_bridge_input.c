#include "lua_bridge_internal.h"

// Auto-repeat state. Declared up here because clearState() has to disarm it;
// the repeat logic itself lives further down next to getButtonsRepeated().
#define REPEAT_BUTTON_BITS 32

static uint32_t s_repeat_delay_ms = 200;  // matches the value every app picked
static uint32_t s_repeat_rate_ms  = 80;
static uint32_t s_repeat_next[REPEAT_BUTTON_BITS];
static bool     s_repeat_reset = false;

// ── picocalc.input.* ─────────────────────────────────────────────────────────

static int l_input_getButtons(lua_State *L) {
  lua_pushinteger(L, kbd_get_buttons());
  return 1;
}

static int l_input_getButtonsPressed(lua_State *L) {
  lua_pushinteger(L, kbd_get_buttons_pressed());
  return 1;
}

static int l_input_getButtonsReleased(lua_State *L) {
  lua_pushinteger(L, kbd_get_buttons_released());
  return 1;
}

static int l_input_getChar(lua_State *L) {
  char c = kbd_get_char();
  if (c) {
    char s[2] = {c, '\0'};
    lua_pushstring(L, s);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

static int l_input_update(lua_State *L) {
  kbd_poll();
  // Bypass the 256-opcode Lua hook latency by serving the system menu
  // instantly if a button press was detected during this explicit update.
  if (kbd_consume_menu_press()) {
    system_menu_show(L);
  }
  return 0;
}

static int l_input_getRawKey(lua_State *L) {
  lua_pushinteger(L, kbd_get_raw_key());
  return 1;
}

static int l_input_clearState(lua_State *L) {
  (void)L;
  kbd_clear_state();
  s_repeat_reset = true;
  return 0;
}

// ── Button auto-repeat ───────────────────────────────────────────────────────
//
// kbd_get_buttons_pressed() reports one edge per physical press and never
// repeats, so apps that want held-to-scroll had to hand-roll a delay/rate timer
// against sys.getTimeMs(). apps/minesweeper, apps/editor, apps/filemanager,
// apps/store and apps/wikipedia each carry their own copy of that logic.
//
// getChar() does repeat (the STM32 firmware emits HOLD events), which is why
// text fields already feel right and only button-driven UIs needed this.
//
// State lives here rather than in keyboard.c so it resets naturally per app:
// lua_bridge_input_init runs on every lua_bridge_register.

static void repeat_reset(void) {
  for (int i = 0; i < REPEAT_BUTTON_BITS; i++) s_repeat_next[i] = 0;
  s_repeat_reset = false;
}

// setRepeat(delayMs, rateMs) — delayMs = 0 disables repeat entirely.
static int l_input_setRepeat(lua_State *L) {
  lua_Integer delay = luaL_checkinteger(L, 1);
  lua_Integer rate  = luaL_optinteger(L, 2, 80);
  if (delay < 0) delay = 0;
  // A zero rate would fire every frame and swamp the caller; 1ms is the floor.
  if (rate < 1) rate = 1;
  s_repeat_delay_ms = (uint32_t)delay;
  s_repeat_rate_ms  = (uint32_t)rate;
  repeat_reset();
  return 0;
}

// getButtonsRepeated() -> mask of real press edges PLUS synthetic repeat edges
// for buttons held past the delay.
//
// Call at most ONCE per frame: it advances the per-button repeat clocks, so a
// second call in the same frame would consume the next repeat early.
static int l_input_getButtonsRepeated(lua_State *L) {
  uint32_t held    = kbd_get_buttons();
  uint32_t pressed = kbd_get_buttons_pressed();
  uint32_t out     = pressed;  // real edges always pass through unchanged

  if (s_repeat_reset) repeat_reset();

  if (s_repeat_delay_ms > 0) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    for (int i = 0; i < REPEAT_BUTTON_BITS; i++) {
      uint32_t bit = 1u << i;

      if (!(held & bit)) {
        s_repeat_next[i] = 0;   // released: disarm
        continue;
      }

      if (pressed & bit) {
        // Fresh press: arm the initial delay. Guard against a 0 deadline,
        // which is the "not armed" sentinel.
        s_repeat_next[i] = now + s_repeat_delay_ms;
        if (s_repeat_next[i] == 0) s_repeat_next[i] = 1;
        continue;
      }

      // Held without a press edge. A zero deadline means we never saw the
      // press — e.g. the key was already down when the app started — so it
      // must not repeat until it is released and pressed again.
      if (s_repeat_next[i] == 0) continue;

      // Signed comparison so this stays correct across the 32-bit ms wrap
      // (~49.7 days uptime).
      if ((int32_t)(now - s_repeat_next[i]) >= 0) {
        out |= bit;
        s_repeat_next[i] = now + s_repeat_rate_ms;
        if (s_repeat_next[i] == 0) s_repeat_next[i] = 1;
      }
    }
  }

  lua_pushinteger(L, out);
  return 1;
}

static const luaL_Reg l_input_lib[] = {
    {"update", l_input_update},
    {"getButtons", l_input_getButtons},
    {"getButtonsPressed", l_input_getButtonsPressed},
    {"getButtonsReleased", l_input_getButtonsReleased},
    {"getChar", l_input_getChar},
    {"getRawKey", l_input_getRawKey},
    {"clearState", l_input_clearState},
    {"setRepeat", l_input_setRepeat},
    {"getButtonsRepeated", l_input_getButtonsRepeated},
    {NULL, NULL}};


void lua_bridge_input_init(lua_State *L) {
  // Runs on every lua_bridge_register, i.e. once per app launch. Restoring the
  // defaults here keeps one app's setRepeat() from leaking into the next.
  s_repeat_delay_ms = 200;
  s_repeat_rate_ms  = 80;
  repeat_reset();

  register_subtable(L, "input", l_input_lib);
  // Push button constants into picocalc.input
  lua_getfield(L, -1, "input");
  lua_pushinteger(L, BTN_UP); lua_setfield(L, -2, "BTN_UP");
  lua_pushinteger(L, BTN_DOWN); lua_setfield(L, -2, "BTN_DOWN");
  lua_pushinteger(L, BTN_LEFT); lua_setfield(L, -2, "BTN_LEFT");
  lua_pushinteger(L, BTN_RIGHT); lua_setfield(L, -2, "BTN_RIGHT");
  lua_pushinteger(L, BTN_ENTER); lua_setfield(L, -2, "BTN_ENTER");
  lua_pushinteger(L, BTN_ESC); lua_setfield(L, -2, "BTN_ESC");
  lua_pushinteger(L, BTN_MENU); lua_setfield(L, -2, "BTN_MENU");
  lua_pushinteger(L, BTN_F1); lua_setfield(L, -2, "BTN_F1");
  lua_pushinteger(L, BTN_F2); lua_setfield(L, -2, "BTN_F2");
  lua_pushinteger(L, BTN_F3); lua_setfield(L, -2, "BTN_F3");
  lua_pushinteger(L, BTN_F4); lua_setfield(L, -2, "BTN_F4");
  lua_pushinteger(L, BTN_F5); lua_setfield(L, -2, "BTN_F5");
  lua_pushinteger(L, BTN_F6); lua_setfield(L, -2, "BTN_F6");
  lua_pushinteger(L, BTN_F7); lua_setfield(L, -2, "BTN_F7");
  lua_pushinteger(L, BTN_F8); lua_setfield(L, -2, "BTN_F8");
  lua_pushinteger(L, BTN_F9); lua_setfield(L, -2, "BTN_F9");
  lua_pushinteger(L, BTN_BACKSPACE); lua_setfield(L, -2, "BTN_BACKSPACE");
  lua_pushinteger(L, BTN_TAB); lua_setfield(L, -2, "BTN_TAB");
  lua_pushinteger(L, BTN_DEL); lua_setfield(L, -2, "BTN_DEL");
  lua_pushinteger(L, BTN_SHIFT); lua_setfield(L, -2, "BTN_SHIFT");
  lua_pushinteger(L, BTN_CTRL); lua_setfield(L, -2, "BTN_CTRL");
  lua_pushinteger(L, BTN_ALT); lua_setfield(L, -2, "BTN_ALT");
  lua_pushinteger(L, BTN_FN); lua_setfield(L, -2, "BTN_FN");
  lua_pop(L, 1);
}
