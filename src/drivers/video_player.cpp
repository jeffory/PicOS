#include "video_player.h"

extern "C" {
#include "sdcard.h"
#include "display.h"
#include "umm_malloc.h"
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pico/time.h>

#include <JPEGDEC.h>

// Simple AVI structure for parsing
typedef struct {
    sdfile_t file;
    uint32_t movi_offset;
    uint32_t movi_size;
    uint32_t next_chunk_pos;
    
    JPEGDEC jpeg;
    uint8_t *jpeg_buffer;
    uint32_t jpeg_buffer_size;
    
    uint64_t start_time_us;
    uint32_t frame_duration_us;
    
    // Performance monitoring
    uint64_t last_frame_times[16];
    int last_frame_idx;
} video_priv_t;

// JPEGDEC drawing callback — pixels are already big-endian (no byte-swap needed)
static int jpeg_draw_cb(JPEGDRAW *pDraw) {
    display_blit_be(pDraw->x, pDraw->y, (const uint16_t *)pDraw->pPixels,
                    pDraw->iWidth, pDraw->iHeight);
    return 1;
}

bool video_player_init(void) {
    return true;
}

video_player_t *video_player_create(void) {
    video_player_t *player = (video_player_t *)umm_malloc(sizeof(video_player_t));
    if (!player) return NULL;
    memset(player, 0, sizeof(video_player_t));
    
    video_priv_t *priv = (video_priv_t *)umm_malloc(sizeof(video_priv_t));
    if (!priv) {
        umm_free(player);
        return NULL;
    }
    memset(priv, 0, sizeof(video_priv_t));
    
    priv->jpeg_buffer_size = 64 * 1024;
    priv->jpeg_buffer = (uint8_t *)umm_malloc(priv->jpeg_buffer_size);
    if (!priv->jpeg_buffer) {
        umm_free(priv);
        umm_free(player);
        return NULL;
    }
    
    player->priv = priv;
    return player;
}

void video_player_destroy(video_player_t *player) {
    if (!player) return;
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (priv) {
        if (priv->file) sdcard_fclose(priv->file);
        if (priv->jpeg_buffer) umm_free(priv->jpeg_buffer);
        umm_free(priv);
    }
    umm_free(player);
}

// Very basic AVI parser - looks for 'movi' list and reads headers
bool video_player_load(video_player_t *player, const char *path) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    priv->file = sdcard_fopen(path, "rb");
    if (!priv->file) return false;
    
    uint8_t header[64];
    sdcard_fread(priv->file, header, 64);
    
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "AVI ", 4) != 0) {
        printf("[VIDEO] Not a valid AVI file\n");
        return false;
    }
    
    // Scan for 'avih' and 'strh' (vids) to get dimensions and FPS
    sdcard_fseek(priv->file, 12);
    uint8_t chunk[8];
    while (sdcard_fread(priv->file, chunk, 8) == 8) {
        uint32_t size = *(uint32_t *)(chunk + 4);
        if (memcmp(chunk, "LIST", 4) == 0) {
            uint8_t list_type[4];
            sdcard_fread(priv->file, list_type, 4);
            if (memcmp(list_type, "movi", 4) == 0) {
                priv->movi_offset = sdcard_ftell(priv->file);
                priv->movi_size = size - 4;
                break;
            }
        } else if (memcmp(chunk, "avih", 4) == 0) {
            uint8_t avih[56];
            sdcard_fread(priv->file, avih, size < 56 ? size : 56);
            uint32_t microsec_per_frame = *(uint32_t *)avih;
            player->fps_num = 1000000;
            player->fps_den = microsec_per_frame;
            player->width = *(uint32_t *)(avih + 32);
            player->height = *(uint32_t *)(avih + 36);
            player->frame_count = *(uint32_t *)(avih + 16);
            priv->frame_duration_us = microsec_per_frame;
            if (size > 56) sdcard_fseek(priv->file, sdcard_ftell(priv->file) + (size - 56));
        } else {
            sdcard_fseek(priv->file, sdcard_ftell(priv->file) + size);
        }
        if (sdcard_ftell(priv->file) & 1) sdcard_fseek(priv->file, sdcard_ftell(priv->file) + 1);
    }
    
    if (priv->movi_offset == 0) {
        printf("[VIDEO] Could not find 'movi' list\n");
        return false;
    }
    
    sdcard_fseek(priv->file, priv->movi_offset);
    priv->next_chunk_pos = priv->movi_offset;
    player->current_frame = 0;

    // Compute centering offsets for partial-region flush
    int vh = (int)player->height;
    if (vh > 320) vh = 320;
    player->y_offset = (uint16_t)((320 - vh) / 2);
    player->visible_height = (uint16_t)vh;

    printf("[VIDEO] Loaded %s: %lux%lu, %u frames, %u fps\n",
           path, player->width, player->height, (unsigned)player->frame_count,
           (unsigned)(player->fps_num / player->fps_den));

    return true;
}

void video_player_play(video_player_t *player) {
    player->playing = true;
    player->paused = false;
    video_priv_t *priv = (video_priv_t *)player->priv;
    priv->start_time_us = time_us_64() - (uint64_t)player->current_frame * priv->frame_duration_us;
}

void video_player_stop(video_player_t *player) {
    player->playing = false;
}

void video_player_pause(video_player_t *player) {
    player->paused = true;
}

void video_player_resume(video_player_t *player) {
    player->paused = false;
    video_priv_t *priv = (video_priv_t *)player->priv;
    priv->start_time_us = time_us_64() - (uint64_t)player->current_frame * priv->frame_duration_us;
}

bool video_player_update(video_player_t *player) {
    if (!player->playing || player->paused) return false;

    video_priv_t *priv = (video_priv_t *)player->priv;
    uint64_t now = time_us_64();
    uint32_t target_frame = (uint32_t)((now - priv->start_time_us) / priv->frame_duration_us);

    if (target_frame <= player->current_frame && player->current_frame != 0) {
        // Busy-wait if next frame is <2ms away instead of returning to Lua
        uint64_t next_us = priv->start_time_us + (uint64_t)(player->current_frame + 1) * priv->frame_duration_us;
        int64_t wait = (int64_t)(next_us - now);
        if (wait > 0 && wait <= 2000) {
            busy_wait_us_32((uint32_t)wait);
            target_frame = player->current_frame; // decode current frame
        } else {
            return false;
        }
    }
    
    bool decoded = false;
    while (player->current_frame <= target_frame) {
        sdcard_fseek(priv->file, priv->next_chunk_pos);
        uint8_t chunk[8];
        if (sdcard_fread(priv->file, chunk, 8) != 8) {
            if (player->loop) {
                video_player_seek(player, 0);
                return false;
            }
            player->playing = false;
            return false;
        }
        
        uint32_t size = *(uint32_t *)(chunk + 4);
        priv->next_chunk_pos = sdcard_ftell(priv->file) + size;
        if (priv->next_chunk_pos & 1) priv->next_chunk_pos++;
        
        if (chunk[2] == 'd' && (chunk[3] == 'b' || chunk[3] == 'c')) {
            if (player->current_frame == target_frame) {
                if (size > priv->jpeg_buffer_size) {
                    priv->jpeg_buffer_size = size + 1024;
                    priv->jpeg_buffer = (uint8_t *)umm_realloc(priv->jpeg_buffer, priv->jpeg_buffer_size);
                }
                sdcard_fread(priv->file, priv->jpeg_buffer, size);
                
                if (priv->jpeg.openRAM(priv->jpeg_buffer, (int)size, jpeg_draw_cb)) {
                    priv->jpeg.setPixelType(RGB565_BIG_ENDIAN);
                    int vw = priv->jpeg.getWidth();
                    int vh = priv->jpeg.getHeight();
                    int x = (vw > 320) ? -(vw - 320) / 2 : (320 - vw) / 2;
                    int y = (vh > 320) ? -(vh - 320) / 2 : (320 - vh) / 2;
                    priv->jpeg.decode(x, y, 0);
                    priv->jpeg.close();
                    decoded = true;

                    // Auto-flush partial region if enabled
                    if (player->auto_flush) {
                        display_flush_region(player->y_offset,
                                             player->y_offset + player->visible_height - 1);
                    }

                    priv->last_frame_times[priv->last_frame_idx] = time_us_64();
                    priv->last_frame_idx = (priv->last_frame_idx + 1) % 16;
                }
            } else {
                sdcard_fseek(priv->file, priv->next_chunk_pos);
            }
            player->current_frame++;
        } else {
            sdcard_fseek(priv->file, priv->next_chunk_pos);
        }
        
        if (player->current_frame >= player->frame_count) {
            if (player->loop) {
                video_player_seek(player, 0);
                return decoded;
            }
            player->playing = false;
            break;
        }
    }
    return decoded;
}

void video_player_seek(video_player_t *player, uint32_t frame) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (frame >= player->frame_count) frame = player->frame_count - 1;
    
    // Simple but slow: reset to start and scan chunks
    // For MJPEG this is actually reasonably fast because we just read headers
    priv->next_chunk_pos = priv->movi_offset;
    player->current_frame = 0;
    
    while (player->current_frame < frame) {
        sdcard_fseek(priv->file, priv->next_chunk_pos);
        uint8_t chunk[8];
        if (sdcard_fread(priv->file, chunk, 8) != 8) break;
        uint32_t size = *(uint32_t *)(chunk + 4);
        priv->next_chunk_pos = sdcard_ftell(priv->file) + size;
        if (priv->next_chunk_pos & 1) priv->next_chunk_pos++;
        if (chunk[2] == 'd' && (chunk[3] == 'b' || chunk[3] == 'c')) {
            player->current_frame++;
        }
    }
    priv->start_time_us = time_us_64() - (uint64_t)player->current_frame * priv->frame_duration_us;
}

float video_player_get_fps(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    int prev_idx = (priv->last_frame_idx + 15) % 16;
    uint64_t dt = priv->last_frame_times[prev_idx] - priv->last_frame_times[priv->last_frame_idx];
    if (dt == 0) return 0;
    return 15000000.0f / (float)dt; // 15 frames difference * 1e6
}
