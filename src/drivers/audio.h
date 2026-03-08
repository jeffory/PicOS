#pragma once

#include <stdint.h>
#include "pico/time.h"
#include "../os/os.h"

void audio_init(void);
void audio_pwm_setup(uint32_t sample_rate);
void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void audio_stop_tone(void);
void audio_set_volume(uint8_t volume);

// Must be called from Core 1 before any audio playback.
// Creates an alarm pool whose timer ISRs fire on Core 1, keeping
// Core 0 free for the app/game loop.
void audio_core1_init(void);

// Returns the Core 1 alarm pool for audio timer registration.
// Used by sound.c and fileplayer.c to ensure their playback timers
// also fire on Core 1.
alarm_pool_t *audio_get_core1_alarm_pool(void);

// PCM sample streaming (for emulators, music players, etc.)
// Samples are stereo interleaved int16_t pairs (L, R, L, R, ...).
// count = number of stereo frames (each frame = 2 int16_t values).
void audio_start_stream(uint32_t sample_rate);
void audio_stop_stream(void);
void audio_push_samples(const int16_t *samples, int count);
