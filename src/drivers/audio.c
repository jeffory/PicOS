#include "audio.h"
#include "../hardware.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "pico/time.h"

#include <stdio.h>

// Alarm pool created on Core 1 — all audio timer ISRs fire on Core 1,
// keeping Core 0 free for the app/game loop.
static alarm_pool_t *s_core1_alarm_pool = NULL;

// --- Tone generation (square wave via PWM frequency modulation) -------------
#define MIN_FREQ 20
#define MAX_FREQ 20000
#define TONE_PWM_WRAP 255

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
  pwm_config_set_wrap(&cfg, TONE_PWM_WRAP);
  pwm_init(s_pwm_slice_l, &cfg, false);
  pwm_init(s_pwm_slice_r, &cfg, false);

  audio_set_volume(100);
}

void audio_core1_init(void) {
  // Hardware alarm 2 (default pool uses 3). 4 slots covers all audio
  // timers: tone, sound sample, fileplayer.
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
  pwm_config_set_wrap(&cfg, TONE_PWM_WRAP);

  uint32_t sys_clk = clock_get_hz(clk_sys);
  uint32_t div = sys_clk / (sample_rate * (TONE_PWM_WRAP + 1));
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
  uint32_t div = sys_clk / (freq_hz * (TONE_PWM_WRAP + 1));
  if (div < 1)
    div = 1;
  if (div > 255)
    div = 255;

  uint16_t level = (TONE_PWM_WRAP + 1) / 2;

  pwm_set_clkdiv(s_pwm_slice_l, div);
  pwm_set_clkdiv(s_pwm_slice_r, div);
  pwm_set_gpio_level(AUDIO_PIN_L, level);
  pwm_set_gpio_level(AUDIO_PIN_R, level);
}

// Logarithmic volume curve: lut[i] = round((10^(i/100) - 1) / 9 * 128), i=0..100
// Replaces runtime exp()/log() with a compile-time table (~5 cycles vs ~100+).
// Values are 0..128 (half of TONE_PWM_WRAP+1=256), matching max_level = (TONE_PWM_WRAP+1)/2.
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
  audio_stop_stream(); // tones and streaming are mutually exclusive

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

// --- PCM sample streaming via DMA paced by PWM DREQ -------------------------
//
// Replaces the old timer-based approach (one ISR per sample = 22k ISR/sec)
// with hardware-paced DMA (one ISR per 256-sample buffer = ~85 ISR/sec).
// The DMA controller autonomously transfers samples to the PWM CC register
// at the exact PWM cycle rate — zero jitter, zero CPU involvement per sample.

#define STREAM_PWM_WRAP   1699
#define STREAM_PWM_MID    ((STREAM_PWM_WRAP + 1) / 2)  // 850
#define STREAM_DMA_SAMPLES 128

#define AUDIO_RING_SIZE 4096 // must be power of 2
#define AUDIO_RING_MASK (AUDIO_RING_SIZE - 1)

static uint8_t s_ring_l[AUDIO_RING_SIZE];
static uint8_t s_ring_r[AUDIO_RING_SIZE];
static volatile uint32_t s_ring_write = 0;
static volatile uint32_t s_ring_read = 0;
static bool s_streaming = false;

static int          s_stream_dma_chan = -1;
static uint32_t     s_stream_dma_buf[2][STREAM_DMA_SAMPLES];
static volatile int s_stream_dma_active_buf = 0;
static volatile bool s_stream_dma_active = false;
static volatile bool s_stream_dma_start_pending = false;
static bool          s_stream_irq_on_core1 = false;
static unsigned int  s_stream_pwm_slice = 0;

static volatile uint32_t s_stream_dma_isr_count = 0;
static volatile uint32_t s_stream_underrun_count = 0;

// Fill one DMA buffer from the ring buffer (called from DMA ISR on Core 1)
static void __time_critical_func(audio_fill_dma_buffer)(uint32_t *buf, int count) {
  uint32_t vol = s_volume_scale;
  for (int i = 0; i < count; i++) {
    uint32_t w = s_ring_write;
    uint32_t r = s_ring_read;
    int32_t lv, rv;

    if (r == w) {
      // Underrun: output silence (midpoint)
      s_stream_underrun_count++;
      lv = STREAM_PWM_MID;
      rv = STREAM_PWM_MID;
    } else {
      uint32_t idx = r & AUDIO_RING_MASK;
      // uint8 [0,255] -> PWM range [0,STREAM_PWM_WRAP] with volume
      lv = ((uint32_t)s_ring_l[idx] * (STREAM_PWM_WRAP + 1)) >> 8;
      rv = ((uint32_t)s_ring_r[idx] * (STREAM_PWM_WRAP + 1)) >> 8;
      lv = (lv * vol) >> 8;
      rv = (rv * vol) >> 8;
      if (lv > STREAM_PWM_WRAP) lv = STREAM_PWM_WRAP;
      if (rv > STREAM_PWM_WRAP) rv = STREAM_PWM_WRAP;
      s_ring_read = r + 1;
    }
    buf[i] = ((uint32_t)rv << 16) | (uint32_t)lv;
  }
}

// DMA completion ISR: swap ping-pong buffers and refill
static void __time_critical_func(audio_stream_dma_isr)(void) {
  s_stream_dma_isr_count++;
  dma_hw->ints0 = 1u << s_stream_dma_chan;

  if (!s_stream_dma_active || !s_streaming) {
    // Stopped: silence outputs, don't restart DMA
    pwm_set_both_levels(s_stream_pwm_slice, STREAM_PWM_MID, STREAM_PWM_MID);
    s_stream_dma_active = false;
    return;
  }

  // Swap to the pre-filled buffer and start DMA immediately
  int next = s_stream_dma_active_buf ^ 1;
  dma_channel_set_read_addr(s_stream_dma_chan, s_stream_dma_buf[next], true);

  // Refill the buffer that just finished playing
  audio_fill_dma_buffer(s_stream_dma_buf[s_stream_dma_active_buf], STREAM_DMA_SAMPLES);
  s_stream_dma_active_buf = next;
}

void audio_start_stream(uint32_t sample_rate) {
  audio_stop_tone();
  if (s_streaming)
    audio_stop_stream();

  // Configure PWM with fractional divider for accurate sample rate
  gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
  gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
  s_stream_pwm_slice = pwm_gpio_to_slice_num(AUDIO_PIN_L);

  uint32_t sys_clk = clock_get_hz(clk_sys);
  uint32_t target = sample_rate * (uint32_t)(STREAM_PWM_WRAP + 1);
  uint32_t div_int = sys_clk / target;
  uint32_t remainder = sys_clk - div_int * target;
  uint32_t div_frac = (remainder * 16 + target / 2) / target;
  if (div_int < 1) { div_int = 1; div_frac = 0; }

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, STREAM_PWM_WRAP);
  pwm_config_set_clkdiv_int_frac(&cfg, div_int, div_frac);
  pwm_init(s_stream_pwm_slice, &cfg, true);

  // Set up DMA channel paced by PWM DREQ
  if (s_stream_dma_chan < 0)
    s_stream_dma_chan = dma_claim_unused_channel(true);

  dma_channel_config dc = dma_channel_get_default_config(s_stream_dma_chan);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, DREQ_PWM_WRAP0 + s_stream_pwm_slice);

  dma_channel_configure(s_stream_dma_chan, &dc,
      &pwm_hw->slice[s_stream_pwm_slice].cc,
      s_stream_dma_buf[0],
      STREAM_DMA_SAMPLES,
      false);  // don't start yet

  // Use DMA_IRQ_0 (mp3_player uses DMA_IRQ_1 — no conflict)
  dma_channel_set_irq0_enabled(s_stream_dma_chan, true);

  // Reset ring buffer and pre-fill DMA buffers with silence
  s_ring_read = 0;
  s_ring_write = 0;
  s_streaming = true;

  audio_fill_dma_buffer(s_stream_dma_buf[0], STREAM_DMA_SAMPLES);
  audio_fill_dma_buffer(s_stream_dma_buf[1], STREAM_DMA_SAMPLES);
  s_stream_dma_active_buf = 0;

  // Signal Core 1 to register IRQ handler and start DMA
  s_stream_dma_start_pending = true;
}

void audio_stop_stream(void) {
  if (!s_streaming)
    return;

  s_stream_dma_start_pending = false;
  if (s_stream_dma_chan >= 0) {
    dma_channel_set_irq0_enabled(s_stream_dma_chan, false);
    dma_channel_abort(s_stream_dma_chan);
  }
  s_stream_dma_active = false;

  // Midpoint is true silence for AC-coupled output (no pop)
  pwm_set_both_levels(s_stream_pwm_slice, STREAM_PWM_MID, STREAM_PWM_MID);
  pwm_set_enabled(s_stream_pwm_slice, false);
  s_streaming = false;
}

void audio_stream_poll(void) {
  if (!s_stream_dma_start_pending)
    return;

  if (!s_stream_irq_on_core1) {
    irq_set_exclusive_handler(DMA_IRQ_0, audio_stream_dma_isr);
    irq_set_enabled(DMA_IRQ_0, true);
    s_stream_irq_on_core1 = true;
  }

  s_stream_dma_active = true;
  dma_channel_start(s_stream_dma_chan);
  s_stream_dma_start_pending = false;
}

void audio_stream_debug(uint32_t *isr_count, uint32_t *underruns, uint32_t *ring_used) {
  if (isr_count) *isr_count = s_stream_dma_isr_count;
  if (underruns) *underruns = s_stream_underrun_count;
  if (ring_used) *ring_used = s_ring_write - s_ring_read;
}

uint32_t audio_ring_free(void) {
  uint32_t used = s_ring_write - s_ring_read;
  if (used > AUDIO_RING_SIZE) return 0; // shouldn't happen
  return AUDIO_RING_SIZE - used;
}

void audio_push_samples(const int16_t *samples, int count) {
  for (int i = 0; i < count; i++) {
    uint32_t avail = s_ring_write - s_ring_read;
    if (avail >= AUDIO_RING_SIZE)
      break; // ring full, drop remaining samples

    int16_t l = samples[i * 2 + 0];
    int16_t r = samples[i * 2 + 1];

    uint32_t idx = s_ring_write & AUDIO_RING_MASK;
    // int16_t [-32768,32767] → uint8_t [0,255]
    s_ring_l[idx] = (uint8_t)((l + 32768) >> 8);
    s_ring_r[idx] = (uint8_t)((r + 32768) >> 8);
    s_ring_write++;
  }
}
