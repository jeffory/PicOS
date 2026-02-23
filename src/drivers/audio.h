#pragma once

#include <stdint.h>
#include "../os/os.h"

void audio_init(void);
void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);
void audio_stop_tone(void);
void audio_set_volume(uint8_t volume);
