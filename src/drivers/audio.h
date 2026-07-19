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
// Uses DMA paced by PWM DREQ — ~750x fewer interrupts than the old timer ISR.
// Samples are stereo interleaved int16_t pairs (L, R, L, R, ...).
// count = number of stereo frames (each frame = 2 int16_t values).
void audio_start_stream(uint32_t sample_rate);
void audio_stop_stream(void);
void audio_push_samples(const int16_t *samples, int count);

// Must be called periodically from Core 1 to handle deferred DMA start.
// This ensures the stream DMA ISR fires on Core 1, not Core 0.
void audio_stream_poll(void);

// Debug: get DMA ISR count, underrun count, and ring buffer fill level.
void audio_stream_debug(uint32_t *isr_count, uint32_t *underruns, uint32_t *ring_used);

// Returns number of free sample slots in the ring buffer.
// Callers should check this before rendering audio to avoid overflow
// (overflow drops samples, causing severe audio corruption).
uint32_t audio_ring_free(void);
