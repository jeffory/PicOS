#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SOUND_MAX_SAMPLES 4
#define SOUND_MAX_SAMPLE_SIZE (64 * 1024)

typedef struct {
    uint8_t *data;
    uint32_t length;
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
    bool loaded;
} sound_sample_t;

typedef struct {
    sound_sample_t *sample;
    bool playing;
    bool paused;
    uint32_t position;
    uint8_t volume;
    uint8_t repeat_count;
    uint8_t repeats_played;
} sound_player_t;

typedef struct {
    sound_sample_t *samples[SOUND_MAX_SAMPLES];
    sound_player_t players[SOUND_MAX_SAMPLES];
    uint8_t active_players;
    uint32_t time_offset_us;
} sound_context_t;

void sound_init(void);
void sound_update(void);

sound_sample_t *sound_sample_create(void);
void sound_sample_destroy(sound_sample_t *sample);
bool sound_sample_load(sound_sample_t *sample, const char *path);
uint32_t sound_sample_get_length(const sound_sample_t *sample);
uint32_t sound_sample_get_sample_rate(const sound_sample_t *sample);

sound_player_t *sound_player_create(void);
void sound_player_destroy(sound_player_t *player);
bool sound_player_set_sample(sound_player_t *player, sound_sample_t *sample);
void sound_player_play(sound_player_t *player, uint8_t repeat_count);
void sound_player_stop(sound_player_t *player);
void sound_player_set_volume(sound_player_t *player, uint8_t volume);
bool sound_player_is_playing(const sound_player_t *player);

uint32_t sound_get_current_time(void);
void sound_reset_time(void);
