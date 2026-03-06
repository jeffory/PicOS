#pragma once

#include <stdint.h>
#include "../os/os.h"

void audio_init(void);
void audio_pwm_setup(uint32_t sample_rate);
void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void audio_stop_tone(void);
void audio_set_volume(uint8_t volume);

// PCM sample streaming (for emulators, music players, etc.)
// Samples are stereo interleaved int16_t pairs (L, R, L, R, ...).
// count = number of stereo frames (each frame = 2 int16_t values).
void audio_start_stream(uint32_t sample_rate);
void audio_stop_stream(void);
void audio_push_samples(const int16_t *samples, int count);
