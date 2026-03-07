#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    bool auto_flush;          // if true, update() calls display_flush_region

    uint16_t y_offset;        // first row of video on screen
    uint16_t visible_height;  // rows of video content

    void *priv; // Internal state (file handle, buffers, etc.)
} video_player_t;

bool video_player_init(void);
video_player_t *video_player_create(void);
void video_player_destroy(video_player_t *player);

bool video_player_load(video_player_t *player, const char *path);
void video_player_play(video_player_t *player);
void video_player_stop(video_player_t *player);
void video_player_pause(video_player_t *player);
void video_player_resume(video_player_t *player);

// Decodes and displays the next frame if it's time
// Returns true if a new frame was decoded to the backbuffer.
bool video_player_update(video_player_t *player);

// Seek to a specific frame
void video_player_seek(video_player_t *player, uint32_t frame);

// Returns actual decoded FPS
float video_player_get_fps(video_player_t *player);

#ifdef __cplusplus
}
#endif
