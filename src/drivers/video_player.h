#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_MAX_FRAME_INDEX 8192
#define VIDEO_FRAME_INDEX_STRIDE 1

#define VIDEO_BUFFER_POOL_SIZE 3
#define VIDEO_MAX_JPEG_SIZE (96 * 1024)

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t frame_count;
    uint32_t current_frame;

    bool playing;
    bool paused;
    bool loop;
    bool auto_flush;

    uint16_t y_offset;
    uint16_t visible_height;

    void *priv;
} video_player_t;

bool video_player_init(void);
video_player_t *video_player_create(void);
void video_player_destroy(video_player_t *player);

bool video_player_load(video_player_t *player, const char *path);
void video_player_play(video_player_t *player);
void video_player_stop(video_player_t *player);
void video_player_pause(video_player_t *player);
void video_player_resume(video_player_t *player);

bool video_player_update(video_player_t *player);
void video_player_seek(video_player_t *player, uint32_t frame);
float video_player_get_fps(video_player_t *player);

uint32_t video_player_get_dropped_frames(video_player_t *player);
void video_player_reset_stats(video_player_t *player);

#ifdef __cplusplus
}
#endif
