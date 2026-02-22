#pragma once

#include <stdint.h>
#include <stdbool.h>

#define FILEPLAYER_BUFFER_SIZE 8192
#define FILEPLAYER_MAX_INSTANCES 2

typedef enum {
    FILEPLAYER_STATE_IDLE = 0,
    FILEPLAYER_STATE_PLAYING,
    FILEPLAYER_STATE_PAUSED,
    FILEPLAYER_STATE_STOPPED
} fileplayer_state_t;

typedef enum {
    FILEPLAYER_TYPE_UNKNOWN = 0,
    FILEPLAYER_TYPE_WAV,
    FILEPLAYER_TYPE_MP3
} fileplayer_type_t;

typedef struct {
    char path[256];
    fileplayer_state_t state;
    fileplayer_type_t type;
    uint32_t position;
    uint32_t length;
    uint8_t volume;
    bool loop;
    uint32_t loop_start;
    uint32_t loop_end;
    int (*finish_callback)(void *);
    void *finish_callback_arg;
} fileplayer_t;

void fileplayer_init(void);
fileplayer_t *fileplayer_create(void);
void fileplayer_destroy(fileplayer_t *player);
bool fileplayer_load(fileplayer_t *player, const char *path);
bool fileplayer_play(fileplayer_t *player, uint8_t repeat_count);
void fileplayer_stop(fileplayer_t *player);
void fileplayer_pause(fileplayer_t *player);
void fileplayer_resume(fileplayer_t *player);
bool fileplayer_is_playing(const fileplayer_t *player);
uint32_t fileplayer_get_position(const fileplayer_t *player);
uint32_t fileplayer_get_length(const fileplayer_t *player);
void fileplayer_set_volume(fileplayer_t *player, uint8_t left, uint8_t right);
void fileplayer_get_volume(const fileplayer_t *player, uint8_t *left, uint8_t *right);
void fileplayer_set_loop_range(fileplayer_t *player, uint32_t start, uint32_t end);
void fileplayer_set_finish_callback(fileplayer_t *player, int (*cb)(void *), void *arg);
void fileplayer_set_offset(fileplayer_t *player, uint32_t seconds);
uint32_t fileplayer_get_offset(const fileplayer_t *player);

void fileplayer_update(void);
bool fileplayer_did_underrun(void);
