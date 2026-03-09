#include "video_player.h"

extern "C" {
#include "sdcard.h"
#include "display.h"
#include "pio_psram.h"
#include "umm_malloc.h"
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pico/time.h>

#include <JPEGDEC.h>

typedef struct {
    uint32_t file_offset;
    uint32_t chunk_size;
} frame_index_entry_t;

typedef struct {
    uint8_t *buffer;       // QMI PSRAM buffer (always allocated for JPEGDEC input)
    uint32_t pio_addr;     // PIO PSRAM address (0 = not using PIO PSRAM)
    uint32_t capacity;
    uint32_t size;
    bool in_use;
    bool on_pio;           // true if frame data is staged in PIO PSRAM
} buffer_pool_entry_t;

// PIO PSRAM layout for video:
// PCM ring uses 0 .. 32KB-1 (from mp3_player).
// Video buffer pool starts after that.
#define VIDEO_PIO_BASE  (32 * 1024)  // skip past PCM ring

typedef struct {
    sdfile_t file;
    uint32_t movi_offset;
    uint32_t movi_size;
    uint32_t next_chunk_pos;

    JPEGDEC jpeg;
    buffer_pool_entry_t buffer_pool[VIDEO_BUFFER_POOL_SIZE];
    int current_buffer;

    uint64_t start_time_us;
    uint32_t frame_duration_us;

    uint64_t last_frame_times[16];
    int last_frame_idx;

    frame_index_entry_t *frame_index;
    uint32_t frame_index_count;
    uint32_t frame_index_capacity;

    uint32_t dropped_frames;
    uint32_t consecutive_drops;
    uint32_t adaptive_stride;

    uint8_t *read_ahead_buffer;
    uint32_t read_ahead_size;
    uint32_t read_ahead_pos;
    uint32_t read_ahead_file_pos;

    // PIO PSRAM read-ahead ring for AVI frame data.
    // When available, raw JPEG frame data is pre-read from SD into PIO PSRAM,
    // then copied to a small SRAM/QMI buffer for JPEGDEC decode.
    bool     pio_ra_active;      // true if PIO PSRAM read-ahead is active
    uint32_t pio_ra_base;        // base address in PIO PSRAM
    uint32_t pio_ra_capacity;    // ring size in PIO PSRAM
    uint32_t pio_ra_wr;          // write position (bytes written from SD)
    uint32_t pio_ra_rd;          // read position (bytes consumed by decode)
    uint32_t pio_ra_file_pos;    // SD file position corresponding to pio_ra_wr
} video_priv_t;

static int jpeg_draw_cb(JPEGDRAW *pDraw) {
    display_blit_be(pDraw->x, pDraw->y, (const uint16_t *)pDraw->pPixels,
                    pDraw->iWidth, pDraw->iHeight);
    return 1;
}

static uint8_t *buffer_pool_acquire(video_priv_t *priv, uint32_t needed_size) {
    for (int i = 0; i < VIDEO_BUFFER_POOL_SIZE; i++) {
        if (!priv->buffer_pool[i].in_use) {
            if (priv->buffer_pool[i].capacity < needed_size) {
                if (priv->buffer_pool[i].buffer) {
                    umm_free(priv->buffer_pool[i].buffer);
                }
                priv->buffer_pool[i].buffer = (uint8_t *)umm_malloc(needed_size);
                if (!priv->buffer_pool[i].buffer) {
                    return NULL;
                }
                priv->buffer_pool[i].capacity = needed_size;
            }
            priv->buffer_pool[i].in_use = true;
            priv->buffer_pool[i].size = needed_size;
            priv->current_buffer = i;
            return priv->buffer_pool[i].buffer;
        }
    }
    return NULL;
}

static void buffer_pool_release(video_priv_t *priv, uint8_t *buffer) {
    for (int i = 0; i < VIDEO_BUFFER_POOL_SIZE; i++) {
        if (priv->buffer_pool[i].buffer == buffer) {
            priv->buffer_pool[i].in_use = false;
            return;
        }
    }
}

static void buffer_pool_cleanup(video_priv_t *priv) {
    for (int i = 0; i < VIDEO_BUFFER_POOL_SIZE; i++) {
        if (priv->buffer_pool[i].buffer) {
            umm_free(priv->buffer_pool[i].buffer);
            priv->buffer_pool[i].buffer = NULL;
            priv->buffer_pool[i].capacity = 0;
            priv->buffer_pool[i].in_use = false;
        }
    }
}

static bool build_frame_index(video_priv_t *priv, video_player_t *player) {
    priv->frame_index_capacity = (player->frame_count / VIDEO_FRAME_INDEX_STRIDE) + 1;
    if (priv->frame_index_capacity > VIDEO_MAX_FRAME_INDEX) {
        priv->frame_index_capacity = VIDEO_MAX_FRAME_INDEX;
    }

    priv->frame_index = (frame_index_entry_t *)umm_malloc(
        priv->frame_index_capacity * sizeof(frame_index_entry_t));
    if (!priv->frame_index) {
        return false;
    }

    uint32_t saved_pos = sdcard_ftell(priv->file);
    sdcard_fseek(priv->file, priv->movi_offset);

    uint32_t frame_num = 0;
    uint32_t index_idx = 0;
    uint8_t chunk[8];

    priv->frame_index[index_idx].file_offset = priv->movi_offset;
    index_idx++;

    while (sdcard_fread(priv->file, chunk, 8) == 8 && frame_num < player->frame_count) {
        uint32_t size = *(uint32_t *)(chunk + 4);
        uint32_t next_pos = sdcard_ftell(priv->file) + size;
        if (next_pos & 1) next_pos++;

        if (chunk[2] == 'd' && (chunk[3] == 'b' || chunk[3] == 'c')) {
            frame_num++;
            if (frame_num % VIDEO_FRAME_INDEX_STRIDE == 0 && index_idx < priv->frame_index_capacity) {
                priv->frame_index[index_idx].file_offset = next_pos;
                index_idx++;
            }
        }
        sdcard_fseek(priv->file, next_pos);
    }

    priv->frame_index_count = index_idx;
    sdcard_fseek(priv->file, saved_pos);
    return true;
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

    // PIO PSRAM has a 27-byte write limit (polpo library design), making it ~70x slower
    // than memcpy for 96KB video frames. Use QMI PSRAM only for video buffers.
    // PIO PSRAM remains available for small transfers (e.g., audio ring buffers).
    bool use_pio = false;
    (void)use_pio;  // suppress unused warning
    
    for (int i = 0; i < VIDEO_BUFFER_POOL_SIZE; i++) {
        // Allocate QMI PSRAM buffer for JPEGDEC input (needs memory-mapped pointer)
        priv->buffer_pool[i].buffer = (uint8_t *)umm_malloc(VIDEO_MAX_JPEG_SIZE);
        if (priv->buffer_pool[i].buffer) {
            priv->buffer_pool[i].capacity = VIDEO_MAX_JPEG_SIZE;
        }
        priv->buffer_pool[i].on_pio = false;
        priv->buffer_pool[i].pio_addr = 0;
    }

    priv->adaptive_stride = 1;

    player->priv = priv;
    return player;
}

void video_player_destroy(video_player_t *player) {
    if (!player) return;
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (priv) {
        if (priv->file) sdcard_fclose(priv->file);
        buffer_pool_cleanup(priv);
        if (priv->frame_index) umm_free(priv->frame_index);
        if (priv->read_ahead_buffer) umm_free(priv->read_ahead_buffer);
        umm_free(priv);
    }
    umm_free(player);
}

bool video_player_load(video_player_t *player, const char *path) {
    video_priv_t *priv = (video_priv_t *)player->priv;

    if (priv->file) {
        sdcard_fclose(priv->file);
        priv->file = NULL;
    }
    if (priv->frame_index) {
        umm_free(priv->frame_index);
        priv->frame_index = NULL;
    }

    priv->file = sdcard_fopen(path, "rb");
    if (!priv->file) return false;

    uint8_t header[64];
    sdcard_fread(priv->file, header, 64);

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "AVI ", 4) != 0) {
        printf("[VIDEO] Not a valid AVI file\n");
        return false;
    }

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

    int vh = (int)player->height;
    if (vh > 320) vh = 320;
    player->y_offset = (uint16_t)((320 - vh) / 2);
    player->visible_height = (uint16_t)vh;

    priv->dropped_frames = 0;
    priv->consecutive_drops = 0;
    priv->adaptive_stride = 1;

    if (!build_frame_index(priv, player)) {
        printf("[VIDEO] Warning: could not build frame index, seeking will be slow\n");
    }

    printf("[VIDEO] Loaded %s: %lux%lu, %u frames, %u fps, %u index entries\n",
           path, player->width, player->height, (unsigned)player->frame_count,
           (unsigned)(player->fps_num / player->fps_den),
           (unsigned)priv->frame_index_count);

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

static bool decode_frame_at(video_player_t *player, video_priv_t *priv, uint32_t chunk_pos, uint32_t size) {
    uint8_t *jpeg_buf = buffer_pool_acquire(priv, size);
    if (!jpeg_buf) {
        sdcard_fseek(priv->file, chunk_pos + 8);
        return false;
    }

    int buf_idx = priv->current_buffer;
    sdcard_fseek(priv->file, chunk_pos + 8);

    if (priv->buffer_pool[buf_idx].on_pio && size <= VIDEO_MAX_JPEG_SIZE) {
        // Stage 1: SD → PIO PSRAM (doesn't touch QMI/XIP cache)
        // Read in chunks since SD reads go through a small SRAM buffer
        uint32_t pio_addr = priv->buffer_pool[buf_idx].pio_addr;
        uint32_t remaining = size;
        uint32_t offset = 0;
        uint8_t sd_chunk[1024];
        while (remaining > 0) {
            uint32_t chunk_len = (remaining > sizeof(sd_chunk)) ? sizeof(sd_chunk) : remaining;
            int rd = sdcard_fread(priv->file, sd_chunk, chunk_len);
            if (rd <= 0) break;
            pio_psram_write(pio_addr + offset, sd_chunk, rd);
            offset += rd;
            remaining -= rd;
        }
        // Stage 2: PIO PSRAM → QMI buffer (for JPEGDEC, which needs a pointer)
        pio_psram_read(pio_addr, jpeg_buf, offset);
    } else {
        // Direct SD → QMI buffer (fallback when PIO PSRAM not available)
        sdcard_fread(priv->file, jpeg_buf, size);
    }

    bool success = false;
    if (priv->jpeg.openRAM(jpeg_buf, (int)size, jpeg_draw_cb)) {
        priv->jpeg.setPixelType(RGB565_BIG_ENDIAN);
        int vw = priv->jpeg.getWidth();
        int vh = priv->jpeg.getHeight();
        int x = (vw > 320) ? -(vw - 320) / 2 : (320 - vw) / 2;
        int y = (vh > 320) ? -(vh - 320) / 2 : (320 - vh) / 2;
        priv->jpeg.decode(x, y, 0);
        priv->jpeg.close();
        success = true;

        if (player->auto_flush) {
            display_flush_region(player->y_offset, player->y_offset + player->visible_height - 1);
        }

        priv->last_frame_times[priv->last_frame_idx] = time_us_64();
        priv->last_frame_idx = (priv->last_frame_idx + 1) % 16;
    }

    buffer_pool_release(priv, jpeg_buf);
    return success;
}

bool video_player_update(video_player_t *player) {
    if (!player->playing || player->paused) return false;

    video_priv_t *priv = (video_priv_t *)player->priv;
    uint64_t now = time_us_64();
    uint32_t target_frame = (uint32_t)((now - priv->start_time_us) / priv->frame_duration_us);

    if (target_frame <= player->current_frame && player->current_frame != 0) {
        uint64_t next_us = priv->start_time_us + (uint64_t)(player->current_frame + 1) * priv->frame_duration_us;
        int64_t wait = (int64_t)(next_us - now);
        if (wait > 0 && wait <= 2000) {
            busy_wait_us_32((uint32_t)wait);
            target_frame = player->current_frame;
        } else {
            return false;
        }
    }

    int32_t frames_behind = (int32_t)(target_frame - player->current_frame);
    if (frames_behind > 3) {
        priv->consecutive_drops++;
        if (priv->consecutive_drops > 2 && priv->adaptive_stride < 4) {
            priv->adaptive_stride++;
            printf("[VIDEO] Increasing stride to %u (behind by %d)\n", priv->adaptive_stride, frames_behind);
        }
    } else {
        if (priv->consecutive_drops > 0) {
            priv->consecutive_drops--;
            if (priv->consecutive_drops == 0 && priv->adaptive_stride > 1) {
                priv->adaptive_stride--;
            }
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
            bool should_decode = (player->current_frame == target_frame);
            int32_t skip_distance = (int32_t)(target_frame - player->current_frame);
            if (!should_decode && skip_distance > 0 && priv->adaptive_stride > 1) {
                if ((player->current_frame % priv->adaptive_stride) == 0) {
                    should_decode = false;
                    priv->dropped_frames++;
                }
            }

            if (should_decode) {
                decoded = decode_frame_at(player, priv, sdcard_ftell(priv->file) - 8, size);
            } else {
                priv->dropped_frames++;
            }
            player->current_frame++;
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

    if (priv->frame_index && priv->frame_index_count > 0) {
        uint32_t target_index = frame / VIDEO_FRAME_INDEX_STRIDE;
        if (target_index >= priv->frame_index_count) {
            target_index = priv->frame_index_count - 1;
        }

        uint32_t start_frame = target_index * VIDEO_FRAME_INDEX_STRIDE;
        priv->next_chunk_pos = priv->frame_index[target_index].file_offset;
        player->current_frame = start_frame;

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
    } else {
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
    }

    priv->start_time_us = time_us_64() - (uint64_t)player->current_frame * priv->frame_duration_us;
    priv->adaptive_stride = 1;
    priv->consecutive_drops = 0;
}

float video_player_get_fps(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    int prev_idx = (priv->last_frame_idx + 15) % 16;
    uint64_t dt = priv->last_frame_times[prev_idx] - priv->last_frame_times[priv->last_frame_idx];
    if (dt == 0) return 0;
    return 15000000.0f / (float)dt;
}

uint32_t video_player_get_dropped_frames(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    return priv->dropped_frames;
}

void video_player_reset_stats(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    priv->dropped_frames = 0;
    priv->consecutive_drops = 0;
    priv->adaptive_stride = 1;
}
