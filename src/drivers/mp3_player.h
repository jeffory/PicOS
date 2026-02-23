#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MP3_WORKING_BUFFER_SIZE 8192

typedef struct {
    void *decoder;
    uint8_t *working_buffer;
    bool playing;
    bool paused;
    uint32_t position;
    uint32_t length;
    uint8_t volume;
    bool loop;
    uint32_t sample_rate;
    uint16_t channels;
} mp3_player_t;

bool mp3_player_init(void);
mp3_player_t *mp3_player_create(void);
void mp3_player_destroy(mp3_player_t *player);
bool mp3_player_load(mp3_player_t *player, const char *path);
bool mp3_player_play(mp3_player_t *player, uint8_t repeat_count);
void mp3_player_stop(mp3_player_t *player);
void mp3_player_pause(mp3_player_t *player);
void mp3_player_resume(mp3_player_t *player);
bool mp3_player_is_playing(const mp3_player_t *player);
uint32_t mp3_player_get_position(const mp3_player_t *player);
uint32_t mp3_player_get_length(const mp3_player_t *player);
void mp3_player_set_volume(mp3_player_t *player, uint8_t volume);
uint8_t mp3_player_get_volume(const mp3_player_t *player);
void mp3_player_set_loop(mp3_player_t *player, bool loop);
void mp3_player_update(void);
