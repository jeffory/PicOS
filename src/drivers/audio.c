#include "audio.h"
#include "../hardware.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/time.h"

#include <stdio.h>

// Alarm pool created on Core 1 — all audio timer ISRs fire on Core 1,
// keeping Core 0 free for the app/game loop.
static alarm_pool_t *s_core1_alarm_pool = NULL;

#define MIN_FREQ 20
#define MAX_FREQ 20000
#define PWM_WRAP 255

static unsigned int s_pwm_slice_l = 0;
static unsigned int s_pwm_slice_r = 0;
static uint8_t s_volume = 100;
static uint32_t s_volume_scale = 256; // 256 = 100%, precomputed for fast scaling
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

void audio_core1_init(void) {
  // Hardware alarm 2 (default pool uses 3). 4 slots covers all audio
  // timers: tone, sound sample, fileplayer, PCM stream.
  s_core1_alarm_pool = alarm_pool_create(2, 4);
  if (!s_core1_alarm_pool) {
    printf("[AUDIO] WARNING: failed to create Core 1 alarm pool\n");
  }
}

alarm_pool_t *audio_get_core1_alarm_pool(void) {
  return s_core1_alarm_pool;
}

void audio_pwm_setup(uint32_t sample_rate) {
  gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
  gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);

  s_pwm_slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
  s_pwm_slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, PWM_WRAP);

  uint32_t sys_clk = clock_get_hz(clk_sys);
  uint32_t div = sys_clk / (sample_rate * (PWM_WRAP + 1));
  if (div < 1) div = 1;
  if (div > 255) div = 255;
  pwm_config_set_clkdiv(&cfg, div);

  pwm_init(s_pwm_slice_l, &cfg, true);
  pwm_init(s_pwm_slice_r, &cfg, true);

  pwm_set_gpio_level(AUDIO_PIN_L, 0);
  pwm_set_gpio_level(AUDIO_PIN_R, 0);
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

// Logarithmic volume curve: lut[i] = round((10^(i/100) - 1) / 9 * 128), i=0..100
// Replaces runtime exp()/log() with a compile-time table (~5 cycles vs ~100+).
// Values are 0..128 (half of PWM_WRAP+1=256), matching max_level = (PWM_WRAP+1)/2.
static const uint8_t s_log_volume_lut[101] = {
    0,   0,   1,   1,   1,   2,   2,   2,   3,   3,   //  0-  9
    4,   4,   5,   5,   5,   6,   6,   7,   7,   8,   // 10- 19
    8,   9,   9,  10,  10,  11,  12,  12,  13,  14,   // 20- 29
   14,  15,  15,  16,  17,  18,  18,  19,  20,  21,   // 30- 39
   22,  22,  23,  24,  25,  26,  27,  28,  29,  30,   // 40- 49
   31,  32,  33,  34,  35,  36,  37,  39,  40,  41,   // 50- 59
   42,  44,  45,  46,  48,  49,  51,  52,  54,  55,   // 60- 69
   57,  59,  60,  62,  64,  66,  68,  70,  71,  73,   // 70- 79
   76,  78,  80,  82,  84,  86,  89,  91,  94,  96,   // 80- 89
   99, 101, 104, 107, 110, 113, 115, 119, 122, 125,   // 90- 99
  128                                                   // 100
};

static void audio_apply_volume(void) {
  if (!s_playing)
    return;

  if (s_volume == 0) {
    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);
  } else {
    uint16_t level = s_log_volume_lut[s_volume];
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
    if (s_core1_alarm_pool) {
      alarm_pool_add_repeating_timer_us(s_core1_alarm_pool, -1000,
                                        audio_timer_callback, NULL, &s_timer);
    } else {
      add_repeating_timer_us(-1000, audio_timer_callback, NULL, &s_timer);
    }
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
  s_volume_scale = (uint32_t)volume * 256 / 100;
  audio_apply_volume();
}

// --- PCM sample streaming via ring buffer + repeating timer -----------------

#define AUDIO_RING_SIZE 8192 // must be power of 2
#define AUDIO_RING_MASK (AUDIO_RING_SIZE - 1)

static uint8_t s_ring_l[AUDIO_RING_SIZE];
static uint8_t s_ring_r[AUDIO_RING_SIZE];
static volatile uint32_t s_ring_write = 0;
static volatile uint32_t s_ring_read = 0;
static repeating_timer_t s_stream_timer;
static bool s_streaming = false;

static bool audio_stream_tick(repeating_timer_t *rt) {
  (void)rt;
  uint32_t w = s_ring_write;
  uint32_t r = s_ring_read;
  if (r == w) {
    // Underrun — hold at midpoint (silence)
    pwm_set_gpio_level(AUDIO_PIN_L, 128);
    pwm_set_gpio_level(AUDIO_PIN_R, 128);
    return true;
  }
  uint32_t idx = r & AUDIO_RING_MASK;
  pwm_set_gpio_level(AUDIO_PIN_L, s_ring_l[idx]);
  pwm_set_gpio_level(AUDIO_PIN_R, s_ring_r[idx]);
  s_ring_read = r + 1;
  return true;
}

void audio_start_stream(uint32_t sample_rate) {
  audio_stop_tone();
  if (s_streaming)
    audio_stop_stream();

  gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
  gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);

  s_pwm_slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
  s_pwm_slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, PWM_WRAP);
  pwm_init(s_pwm_slice_l, &cfg, true);
  pwm_init(s_pwm_slice_r, &cfg, true);

  pwm_set_gpio_level(AUDIO_PIN_L, 128);
  pwm_set_gpio_level(AUDIO_PIN_R, 128);

  s_ring_read = 0;
  s_ring_write = 0;
  s_streaming = true;

  // Negative period = fixed interval regardless of callback duration
  int32_t period_us = -(int32_t)(1000000 / sample_rate);
  if (s_core1_alarm_pool) {
    alarm_pool_add_repeating_timer_us(s_core1_alarm_pool, period_us,
                                      audio_stream_tick, NULL, &s_stream_timer);
  } else {
    add_repeating_timer_us(period_us, audio_stream_tick, NULL, &s_stream_timer);
  }
}

void audio_stop_stream(void) {
  if (!s_streaming)
    return;
  cancel_repeating_timer(&s_stream_timer);
  pwm_set_gpio_level(AUDIO_PIN_L, 0);
  pwm_set_gpio_level(AUDIO_PIN_R, 0);
  pwm_set_enabled(s_pwm_slice_l, false);
  pwm_set_enabled(s_pwm_slice_r, false);
  s_streaming = false;
}

void audio_stream_poll(void) {
  // No-op: timer-based streaming doesn't need deferred start.
  // Kept for API compatibility with Core 1 loop in main.c.
}

void audio_push_samples(const int16_t *samples, int count) {
  for (int i = 0; i < count; i++) {
    uint32_t avail = s_ring_write - s_ring_read;
    if (avail >= AUDIO_RING_SIZE)
      break; // ring full, drop remaining samples

    int16_t l = samples[i * 2 + 0];
    int16_t r = samples[i * 2 + 1];

    // Apply master volume via precomputed multiply+shift (avoids division)
    l = (int16_t)(((int32_t)l * (int32_t)s_volume_scale) >> 8);
    r = (int16_t)(((int32_t)r * (int32_t)s_volume_scale) >> 8);

    uint32_t idx = s_ring_write & AUDIO_RING_MASK;
    // int16_t [-32768,32767] → uint8_t [0,255] for PWM
    s_ring_l[idx] = (uint8_t)((l + 32768) >> 8);
    s_ring_r[idx] = (uint8_t)((r + 32768) >> 8);
    s_ring_write++;
  }
}
