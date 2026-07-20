#include "idle_dim.h"
#include "../drivers/keyboard.h"

#include "pico/time.h"

// Backlight level while dimmed. Low enough to stop burn-in but the screen
// stays faintly readable so the device doesn't look switched off.
#define IDLE_DIM_LEVEL 10

static uint32_t s_timeout_ms = 60000; // 0 = disabled
static uint32_t s_last_activity_ms = 0;
static uint8_t s_user_brightness = 128;
static bool s_dimmed = false;

void idle_dim_init(uint8_t user_brightness, uint32_t timeout_s) {
  s_user_brightness = user_brightness;
  s_timeout_ms = timeout_s * 1000u;
  s_last_activity_ms = to_ms_since_boot(get_absolute_time());
  s_dimmed = false;
}

void idle_dim_set_brightness(uint8_t brightness) {
  s_user_brightness = brightness;
  s_dimmed = false; // the user is actively adjusting — screen is awake
}

bool idle_dim_note_activity(void) {
  s_last_activity_ms = to_ms_since_boot(get_absolute_time());
  if (s_dimmed) {
    s_dimmed = false;
    kbd_set_backlight(s_user_brightness);
    return true;
  }
  return false;
}

void idle_dim_poll(void) {
  if (s_dimmed || s_timeout_ms == 0)
    return;
  uint32_t now = to_ms_since_boot(get_absolute_time());
  if (now - s_last_activity_ms >= s_timeout_ms) {
    s_dimmed = true;
    uint8_t dim = s_user_brightness < IDLE_DIM_LEVEL ? s_user_brightness
                                                     : IDLE_DIM_LEVEL;
    kbd_set_backlight(dim);
  }
}

bool idle_dim_is_dimmed(void) { return s_dimmed; }
