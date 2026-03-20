#include "video_player.h"

extern "C" {
#include "sdcard.h"
#include "display.h"
#include "umm_malloc.h"
#include "wifi.h"
#include "../os/launcher.h"
#include "../os/config.h"
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pico/time.h>
#include <hardware/watchdog.h>

#include <JPEGDEC.h>

typedef struct {
    uint32_t file_offset;
    uint32_t chunk_size;
} frame_index_entry_t;

typedef struct {
    uint32_t ra_offset;   // byte offset within the ring buffer
    uint32_t size;        // JPEG payload size
    bool     valid;
} ra_frame_entry_t;

typedef struct {
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t size;
    bool in_use;
} buffer_pool_entry_t;

typedef struct {
    sdfile_t file;
    uint32_t movi_offset;
    uint32_t movi_size;
    uint32_t next_chunk_pos;

    JPEGDEC *jpeg;       // points to static s_jpeg_sram (SRAM BSS)
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

    // Read-ahead ring buffer (QMI PSRAM)
    uint8_t         *ra_buffer;        // ring buffer base
    uint32_t         ra_capacity;      // buffer size in bytes
    ra_frame_entry_t *ra_frames;       // per-frame metadata array
    uint32_t         ra_frame_count;   // number of frames buffered
    uint32_t         ra_first_frame;   // lowest frame number in buffer
    uint32_t         ra_hits;          // diagnostic: cache hits
    uint32_t         ra_misses;        // diagnostic: cache misses

    // Deferred flush: DMA starts at the beginning of the next decode,
    // overlapping with the SD read for the next frame.
    bool     pending_flush;
    uint16_t flush_y0;
    uint16_t flush_y1;

    bool     overclocked;       // True if we boosted the clock for playback
    bool     wifi_was_connected; // True if WiFi was connected before overclock
} video_priv_t;

// JPEGDEC in static SRAM BSS: Huffman tables (10KB), MCU buffers, VLC staging,
// and quant tables all get single-cycle SRAM access instead of going through
// the 16KB XIP cache (which thrashes against 24KB of JPEGDEC flash code).
static JPEGDEC s_jpeg_sram;

// --- Adaptive quality: 2x nearest-neighbor upscale callback ---
// When the player falls behind, we decode at half resolution and upscale
// each MCU row directly to the framebuffer via this callback.
static int s_adaptive_x_offset;
static int s_adaptive_y_offset;

static int jpeg_draw_cb(JPEGDRAW *pDraw) {
    display_blit_be(pDraw->x, pDraw->y, (const uint16_t *)pDraw->pPixels,
                    pDraw->iWidth, pDraw->iHeight);
    return 1;
}

static int jpeg_draw_cb_2x(JPEGDRAW *pDraw) {
    uint16_t *fb = display_get_back_buffer();
    for (int row = 0; row < pDraw->iHeight; row++) {
        const uint16_t *src = &pDraw->pPixels[row * pDraw->iWidth];
        int dy = s_adaptive_y_offset + (pDraw->y + row) * 2;
        if (dy < 0 || dy + 1 >= FB_HEIGHT) continue;
        uint16_t *dst0 = &fb[dy * FB_WIDTH];
        uint16_t *dst1 = &fb[(dy + 1) * FB_WIDTH];
        for (int col = 0; col < pDraw->iWidth; col++) {
            int dx = s_adaptive_x_offset + (pDraw->x + col) * 2;
            if (dx < 0 || dx + 1 >= FB_WIDTH) continue;
            uint16_t pixel = src[col];
            dst0[dx]     = pixel;
            dst0[dx + 1] = pixel;
            dst1[dx]     = pixel;
            dst1[dx + 1] = pixel;
        }
    }
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

// Build a complete frame index (one entry per frame) for O(1) seeking
// and instant skip-to-target in the update loop.
static bool build_frame_index(video_priv_t *priv, video_player_t *player) {
    priv->frame_index_capacity = player->frame_count;
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
    uint8_t chunk[8];

    while (sdcard_fread(priv->file, chunk, 8) == 8 && frame_num < priv->frame_index_capacity) {
        uint32_t chunk_pos = sdcard_ftell(priv->file) - 8;
        uint32_t size = *(uint32_t *)(chunk + 4);
        uint32_t next_pos = sdcard_ftell(priv->file) + size;
        if (next_pos & 1) next_pos++;

        if (chunk[2] == 'd' && (chunk[3] == 'b' || chunk[3] == 'c')) {
            priv->frame_index[frame_num].file_offset = chunk_pos;
            priv->frame_index[frame_num].chunk_size = size;
            frame_num++;
            if (frame_num % 100 == 0) watchdog_update();
        }
        sdcard_fseek(priv->file, next_pos);
    }

    priv->frame_index_count = frame_num;
    sdcard_fseek(priv->file, saved_pos);
    return true;
}

#define RA_BUFFER_SIZE  (1024 * 1024)  // 1MB
#define RA_MAX_FRAMES   256

// Bulk-read consecutive frames from SD into the QMI PSRAM ring buffer.
static void ra_fill_from(video_priv_t *priv, uint32_t start_frame, uint32_t max_frames) {
    if (!priv->ra_buffer || !priv->ra_frames || !priv->frame_index) return;

    uint64_t t_start = time_us_64();
    uint32_t offset = 0;
    uint32_t count = 0;
    uint32_t saved_pos = sdcard_ftell(priv->file);

    priv->ra_first_frame = start_frame;
    priv->ra_frame_count = 0;

    for (uint32_t i = start_frame; i < priv->frame_index_count && count < max_frames; i++) {
        uint32_t size = priv->frame_index[i].chunk_size;
        if (offset + size > priv->ra_capacity) break;

        uint32_t file_pos = priv->frame_index[i].file_offset + 8; // skip chunk header
        sdcard_fseek(priv->file, file_pos);
        sdcard_fread(priv->file, priv->ra_buffer + offset, size);

        priv->ra_frames[count].ra_offset = offset;
        priv->ra_frames[count].size = size;
        priv->ra_frames[count].valid = true;

        offset += size;
        count++;
        if (count % 20 == 0) watchdog_update();
    }

    priv->ra_frame_count = count;
    sdcard_fseek(priv->file, saved_pos);

    uint64_t elapsed_ms = (time_us_64() - t_start) / 1000;
    printf("[VIDEO] Read-ahead: %u frames (%uKB) in %ums\n",
           (unsigned)count, (unsigned)(offset / 1024), (unsigned)elapsed_ms);
}

// Flush any deferred frame to the display.
static void flush_pending(video_priv_t *priv) {
    if (priv->pending_flush) {
        display_flush_region(priv->flush_y0, priv->flush_y1);
        priv->pending_flush = false;
    }
}

static void video_boost_clock(video_priv_t *priv);
static void video_restore_clock(video_priv_t *priv);

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

    priv->jpeg = &s_jpeg_sram;

    for (int i = 0; i < VIDEO_BUFFER_POOL_SIZE; i++) {
        priv->buffer_pool[i].buffer = (uint8_t *)umm_malloc(VIDEO_MAX_JPEG_SIZE);
        if (priv->buffer_pool[i].buffer) {
            priv->buffer_pool[i].capacity = VIDEO_MAX_JPEG_SIZE;
        }
    }

    priv->adaptive_stride = 1;

    player->priv = priv;
    return player;
}

void video_player_destroy(video_player_t *player) {
    if (!player) return;
    video_priv_t *priv = (video_priv_t *)player->priv;
    if (priv) {
        flush_pending(priv);
        video_restore_clock(priv);
        if (priv->file) sdcard_fclose(priv->file);
        buffer_pool_cleanup(priv);
        if (priv->frame_index) umm_free(priv->frame_index);
        if (priv->ra_buffer) umm_free(priv->ra_buffer);
        if (priv->ra_frames) umm_free(priv->ra_frames);
        // priv->jpeg points to static s_jpeg_sram — no free needed
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
        watchdog_update();
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
    int vw = (int)player->width;
    // Account for auto-downscaling in decode_frame_at
    if (vw > 640 || vh > 640) {
        vh /= 4;
    } else if (vw > 320 || vh > 320) {
        vh /= 2;
    }
    if (vh < 1) vh = 1;
    if (vh > 320) vh = 320;
    player->y_offset = (uint16_t)((320 - vh) / 2);
    player->visible_height = (uint16_t)vh;

    priv->dropped_frames = 0;
    priv->consecutive_drops = 0;
    priv->adaptive_stride = 1;
    priv->pending_flush = false;

    if (!build_frame_index(priv, player)) {
        printf("[VIDEO] Warning: could not build frame index, seeking will be slow\n");
    }

    // Allocate read-ahead buffer in QMI PSRAM
    priv->ra_buffer = (uint8_t *)umm_malloc(RA_BUFFER_SIZE);
    if (priv->ra_buffer) {
        priv->ra_capacity = RA_BUFFER_SIZE;
        priv->ra_frames = (ra_frame_entry_t *)umm_malloc(RA_MAX_FRAMES * sizeof(ra_frame_entry_t));
        if (!priv->ra_frames) {
            umm_free(priv->ra_buffer);
            priv->ra_buffer = NULL;
        } else {
            ra_fill_from(priv, 0, RA_MAX_FRAMES);
        }
    }
    if (!priv->ra_buffer) {
        printf("[VIDEO] Read-ahead: allocation failed, continuing without cache\n");
    }

    printf("[VIDEO] Loaded %s: %lux%lu, %u frames, %u fps, %u index entries\n",
           path, player->width, player->height, (unsigned)player->frame_count,
           (unsigned)(player->fps_num / player->fps_den),
           (unsigned)priv->frame_index_count);

    return true;
}

static void video_boost_clock(video_priv_t *priv) {
    if (priv->overclocked) return;

    // The CYW43 PIO SPI clock divider is set at init (200 MHz) and is NOT
    // updated by launcher_apply_clock(), so WiFi must be disconnected first.
    priv->wifi_was_connected = false;
    if (wifi_is_available()) {
        wifi_status_t wst = wifi_get_status();
        if (wst == WIFI_STATUS_CONNECTED || wst == WIFI_STATUS_CONNECTING) {
            priv->wifi_was_connected = true;
            wifi_disconnect();
            sleep_ms(50); // Let Core 1 process the disconnect
        }
    }

    // Boost to 300 MHz for video playback
    launcher_apply_clock(300000);
    priv->overclocked = true;
}

static void video_restore_clock(video_priv_t *priv) {
    if (!priv->overclocked) return;

    launcher_apply_clock(200000);
    priv->overclocked = false;

    // Reconnect WiFi if it was connected before
    if (priv->wifi_was_connected && wifi_is_available()) {
        const char *ssid = config_get("wifi_ssid");
        const char *pass = config_get("wifi_pass");
        if (ssid && ssid[0])
            wifi_connect(ssid, pass ? pass : "");
        priv->wifi_was_connected = false;
    }
}

void video_player_play(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    video_boost_clock(priv);
    player->playing = true;
    player->paused = false;
    priv->start_time_us = time_us_64() - (uint64_t)player->current_frame * priv->frame_duration_us;
}

void video_player_stop(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    flush_pending(priv);
    player->playing = false;
    video_restore_clock(priv);
}

void video_player_pause(video_player_t *player) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    flush_pending(priv);
    player->paused = true;
}

void video_player_resume(video_player_t *player) {
    player->paused = false;
    video_priv_t *priv = (video_priv_t *)player->priv;
    priv->start_time_us = time_us_64() - (uint64_t)player->current_frame * priv->frame_duration_us;
}

static bool decode_frame_at(video_player_t *player, video_priv_t *priv,
                            uint32_t target_frame, uint32_t chunk_pos, uint32_t size) {
    uint8_t *jpeg_buf = NULL;
    bool from_cache = false;

    // Check read-ahead buffer
    if (priv->ra_buffer && priv->ra_frame_count > 0
        && target_frame >= priv->ra_first_frame
        && target_frame < priv->ra_first_frame + priv->ra_frame_count) {
        uint32_t idx = target_frame - priv->ra_first_frame;
        if (priv->ra_frames[idx].valid) {
            jpeg_buf = priv->ra_buffer + priv->ra_frames[idx].ra_offset;
            size = priv->ra_frames[idx].size;
            from_cache = true;
            priv->ra_hits++;
        }
    }

    // Fallback: read from SD
    if (!from_cache) {
        priv->ra_misses++;
        jpeg_buf = buffer_pool_acquire(priv, size);
        if (!jpeg_buf) {
            return false;
        }
    }

    uint64_t t_sd_start = time_us_64();
    uint64_t t_sd_end = t_sd_start;

    if (!from_cache) {
        // Read JPEG data from SD (SPI0).
        // Previous frame's flush was already issued at the top of video_player_update(),
        // and DMA (PIO0) likely finished during the target-frame calculation + SD seek.
        sdcard_fseek(priv->file, chunk_pos + 8);
        sdcard_fread(priv->file, jpeg_buf, size);
        t_sd_end = time_us_64();
    }

    // Wait for any in-flight DMA to finish before writing to the back buffer.
    display_wait_for_flush();
    uint64_t t_flush_wait = time_us_64();

    bool success = false;
    bool use_adaptive = (priv->adaptive_stride > 1);

    JPEG_DRAW_CALLBACK *draw_cb = use_adaptive ? jpeg_draw_cb_2x : jpeg_draw_cb;

    uint64_t t_open_start = time_us_64();
    if (priv->jpeg->openRAM(jpeg_buf, (int)size, draw_cb)) {
        uint64_t t_setup_start = time_us_64();
        priv->jpeg->setPixelType(RGB565_BIG_ENDIAN);
        int vw = priv->jpeg->getWidth();
        int vh = priv->jpeg->getHeight();
        int scale = 0;
        if (vw > 640 || vh > 640) {
            scale = JPEG_SCALE_QUARTER;
            vw /= 4; vh /= 4;
        } else if (vw > 320 || vh > 320) {
            scale = JPEG_SCALE_HALF;
            vw /= 2; vh /= 2;
        }

        // Step 5: Adaptive quality — decode at one extra level of downscale
        // and use the 2x upscale callback.  Drops decode time ~60%.
        if (use_adaptive && scale < JPEG_SCALE_QUARTER) {
            if (scale == 0) {
                scale = JPEG_SCALE_HALF;
                vw /= 2; vh /= 2;
            } else if (scale == JPEG_SCALE_HALF) {
                scale = JPEG_SCALE_QUARTER;
                vw /= 2; vh /= 2;
            }
            // Offsets for the 2x upscale callback to center the output
            s_adaptive_x_offset = (320 - vw * 2) / 2;
            s_adaptive_y_offset = (320 - vh * 2) / 2;
            priv->jpeg->decode(0, 0, scale);
        }
        // Step 4: Direct framebuffer decode for 320-wide videos.
        // JPEGDEC pitch = iCropCX = decoded width, which matches FB_WIDTH
        // when vw == 320, so setFramebuffer works correctly.
        else if (vw == 320 && scale == 0) {
            uint16_t *back_buf = display_get_back_buffer();
            int y = (320 - vh) / 2;
            priv->jpeg->setFramebuffer(back_buf + y * FB_WIDTH);
            priv->jpeg->decode(0, 0, scale);
        }
        // Standard callback path for non-320-wide videos
        else {
            int x = (320 - vw) / 2;
            int y = (320 - vh) / 2;
            priv->jpeg->decode(x, y, scale);
        }

        uint64_t t_dec_end = time_us_64();
        priv->jpeg->close();
        uint64_t t_close = time_us_64();
        success = true;

        // Print timing every 30 frames — split decode into open (header/Huffman parse)
        // vs decode (IDCT + pixel conversion) to identify the hot spot.
        if (player->current_frame % 30 == 0) {
            printf("[VIDEO] f=%u sd=%ums flush=%ums open=%ums dec=%ums close=%ums total=%ums stride=%u src=%s ra=%u/%u\n",
                   (unsigned)player->current_frame,
                   (unsigned)((t_sd_end - t_sd_start) / 1000),
                   (unsigned)((t_flush_wait - t_sd_end) / 1000),
                   (unsigned)((t_setup_start - t_open_start) / 1000),
                   (unsigned)((t_dec_end - t_setup_start) / 1000),
                   (unsigned)((t_close - t_dec_end) / 1000),
                   (unsigned)((t_close - t_sd_start) / 1000),
                   (unsigned)priv->adaptive_stride,
                   from_cache ? "cache" : "sd",
                   (unsigned)priv->ra_hits,
                   (unsigned)(priv->ra_hits + priv->ra_misses));
        }

        // Defer the flush — it will be issued at the start of the next
        // video_player_update(), hiding DMA wait behind target-frame calc + SD read.
        if (player->auto_flush) {
            priv->pending_flush = true;
            priv->flush_y0 = player->y_offset;
            priv->flush_y1 = player->y_offset + player->visible_height - 1;
        }

        priv->last_frame_times[priv->last_frame_idx] = time_us_64();
        priv->last_frame_idx = (priv->last_frame_idx + 1) % 16;
    }

    if (!from_cache) {
        buffer_pool_release(priv, jpeg_buf);
    }
    return success;
}

bool video_player_update(video_player_t *player) {
    if (!player->playing || player->paused) return false;

    video_priv_t *priv = (video_priv_t *)player->priv;

    // Flush any deferred frame from the previous update.
    // This fires early so DMA runs during target-frame calculation,
    // and prevents double-swap jitter if Lua also called disp.flush().
    flush_pending(priv);

    uint64_t now = time_us_64();
    uint32_t target_frame = (uint32_t)((now - priv->start_time_us) / priv->frame_duration_us);

    if (target_frame < player->current_frame) {
        // Wall-clock says we should be showing a frame we already decoded.
        // Wait for the next frame (current_frame) to become due.
        uint64_t next_us = priv->start_time_us + (uint64_t)player->current_frame * priv->frame_duration_us;
        int64_t wait = (int64_t)(next_us - now);
        if (wait > 0 && wait <= 5000) {
            busy_wait_us_32((uint32_t)wait);
            target_frame = player->current_frame;
        } else {
            return false;
        }
    }

    // Handle end of video
    if (target_frame >= player->frame_count) {
        if (player->loop) {
            flush_pending(priv);
            video_player_seek(player, 0);
            return false;
        }
        flush_pending(priv);
        player->playing = false;
        return false;
    }

    // Track frame drops and adapt stride
    int32_t frames_behind = (int32_t)(target_frame - player->current_frame);
    if (frames_behind > 3) {
        priv->consecutive_drops++;
        if (priv->consecutive_drops > 2 && priv->adaptive_stride < 4) {
            priv->adaptive_stride++;
            printf("[VIDEO] Increasing stride to %u (behind by %d)\n",
                   (unsigned)priv->adaptive_stride, (int)frames_behind);
        }
        priv->dropped_frames += (uint32_t)frames_behind;
    } else {
        if (priv->consecutive_drops > 0) {
            priv->consecutive_drops--;
            if (priv->consecutive_drops == 0 && priv->adaptive_stride > 1) {
                priv->adaptive_stride--;
                printf("[VIDEO] Decreasing stride to %u\n", (unsigned)priv->adaptive_stride);
            }
        }
    }

    bool decoded = false;

    // Fast path: direct index lookup — skip to the target frame in O(1).
    // No sequential chunk header scanning needed.
    if (priv->frame_index && target_frame < priv->frame_index_count) {
        uint32_t chunk_pos = priv->frame_index[target_frame].file_offset;
        uint32_t size = priv->frame_index[target_frame].chunk_size;

        decoded = decode_frame_at(player, priv, target_frame, chunk_pos, size);

        player->current_frame = target_frame + 1;
        priv->next_chunk_pos = chunk_pos + 8 + size;
        if (priv->next_chunk_pos & 1) priv->next_chunk_pos++;
    } else {
        // Slow fallback: sequential chunk reading (no index or frame beyond index)
        while (player->current_frame <= target_frame) {
            sdcard_fseek(priv->file, priv->next_chunk_pos);
            uint8_t chunk[8];
            if (sdcard_fread(priv->file, chunk, 8) != 8) {
                if (player->loop) {
                    flush_pending(priv);
                    video_player_seek(player, 0);
                    return false;
                }
                flush_pending(priv);
                player->playing = false;
                return false;
            }

            uint32_t size = *(uint32_t *)(chunk + 4);
            priv->next_chunk_pos = sdcard_ftell(priv->file) + size;
            if (priv->next_chunk_pos & 1) priv->next_chunk_pos++;

            if (chunk[2] == 'd' && (chunk[3] == 'b' || chunk[3] == 'c')) {
                if (player->current_frame == target_frame) {
                    decoded = decode_frame_at(player, priv, target_frame, sdcard_ftell(priv->file) - 8, size);
                } else {
                    priv->dropped_frames++;
                }
                player->current_frame++;
            }

            if (player->current_frame >= player->frame_count) {
                if (player->loop) {
                    flush_pending(priv);
                    video_player_seek(player, 0);
                    return decoded;
                }
                flush_pending(priv);
                player->playing = false;
                break;
            }
        }
    }
    return decoded;
}

void video_player_seek(video_player_t *player, uint32_t frame) {
    video_priv_t *priv = (video_priv_t *)player->priv;
    flush_pending(priv);

    if (frame >= player->frame_count) frame = player->frame_count - 1;

    // Fast path: direct index lookup
    if (priv->frame_index && frame < priv->frame_index_count) {
        priv->next_chunk_pos = priv->frame_index[frame].file_offset;
        player->current_frame = frame;
    } else {
        // Slow fallback: scan from beginning
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
                if (player->current_frame % 100 == 0) watchdog_update();
            }
        }
    }

    priv->start_time_us = time_us_64() - (uint64_t)player->current_frame * priv->frame_duration_us;
    priv->adaptive_stride = 1;
    priv->consecutive_drops = 0;
    priv->ra_frame_count = 0;  // invalidate read-ahead on seek
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
