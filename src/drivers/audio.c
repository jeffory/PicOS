#include "audio.h"
#include "../hardware.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/time.h"

#include <math.h>

#define MIN_FREQ 20
#define MAX_FREQ 20000
#define PWM_WRAP 255

static unsigned int s_pwm_slice_l = 0;
static unsigned int s_pwm_slice_r = 0;
static uint8_t s_volume = 100;
static bool s_playing = false;
static repeating_timer_t s_timer;

static uint64_t s_end_time_us = 0;

static bool audio_timer_callback(repeating_timer_t *rt) {
  (void)rt;
  if (s_end_time_us > 0 && time_us_64() >= s_end_time_us) {
    audio_stop_tone();
    return false;
  }
  return true;
}

void audio_init(void) {
  gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
  gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);

  s_pwm_slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
  s_pwm_slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, PWM_WRAP);
  pwm_init(s_pwm_slice_l, &cfg, false);
  pwm_init(s_pwm_slice_r, &cfg, false);

  audio_set_volume(100);
}

static void audio_configure_freq(uint32_t freq_hz) {
  if (freq_hz < MIN_FREQ)
    freq_hz = MIN_FREQ;
  if (freq_hz > MAX_FREQ)
    freq_hz = MAX_FREQ;

  uint32_t sys_clk = clock_get_hz(clk_sys);
  uint32_t div = sys_clk / (freq_hz * (PWM_WRAP + 1));
  if (div < 1)
    div = 1;
  if (div > 255)
    div = 255;

  uint16_t level = (PWM_WRAP + 1) / 2;

  pwm_set_clkdiv(s_pwm_slice_l, div);
  pwm_set_clkdiv(s_pwm_slice_r, div);
  pwm_set_gpio_level(AUDIO_PIN_L, level);
  pwm_set_gpio_level(AUDIO_PIN_R, level);
}

static void audio_apply_volume(void) {
  if (!s_playing)
    return;

  uint16_t max_level = (PWM_WRAP + 1) / 2;
  if (s_volume == 0) {
    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);
  } else {
    double log_vol = (exp(s_volume / 100.0 * log(10)) - 1) / 9.0;
    uint16_t level = (uint16_t)(max_level * log_vol);
    if (level > max_level)
      level = max_level;
    pwm_set_gpio_level(AUDIO_PIN_L, level);
    pwm_set_gpio_level(AUDIO_PIN_R, level);
  }
}

void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
  if (freq_hz < MIN_FREQ)
    freq_hz = MIN_FREQ;
  if (freq_hz > MAX_FREQ)
    freq_hz = MAX_FREQ;

  audio_stop_tone();

  audio_configure_freq(freq_hz);
  audio_apply_volume();

  pwm_set_enabled(s_pwm_slice_l, true);
  pwm_set_enabled(s_pwm_slice_r, true);

  s_playing = true;

  if (duration_ms > 0) {
    s_end_time_us = time_us_64() + (duration_ms * 1000);
    add_repeating_timer_us(-1000, audio_timer_callback, NULL, &s_timer);
  } else {
    s_end_time_us = 0;
  }
}

void audio_stop_tone(void) {
  cancel_repeating_timer(&s_timer);
  s_end_time_us = 0;

  pwm_set_enabled(s_pwm_slice_l, false);
  pwm_set_enabled(s_pwm_slice_r, false);
  s_playing = false;
}

void audio_set_volume(uint8_t volume) {
  if (volume > 100)
    volume = 100;
  s_volume = volume;
  audio_apply_volume();
}
