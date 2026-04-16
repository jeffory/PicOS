#include "fileplayer.h"
#include "audio.h"
#include "../hardware.h"
#include "sdcard.h"
#include "ff.h"       // direct FatFS calls for non-blocking SD reads
#include "pico/stdlib.h"
#include "mp3_player.h"
#include "umm_malloc.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define WAV_BUFFER_SIZE FILEPLAYER_BUFFER_SIZE

static fileplayer_t s_players[FILEPLAYER_MAX_INSTANCES];
static fileplayer_t *s_active_player = NULL;
static uint32_t s_sample_rate = 44100;
static uint8_t s_volume_l = 100;
static uint8_t s_volume_r = 100;
static bool s_initialized = false;
static sdfile_t s_current_file = NULL;
static uint8_t *s_wav_buffer = NULL;
static volatile bool s_underflow = false;

static bool parse_wav_header(sdfile_t f, uint32_t *sample_rate, uint16_t *channels, uint16_t *bits_per_sample, uint32_t *data_size) {
    uint8_t header[44];
    if (sdcard_fread(f, header, 44) < 44) {
        return false;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }

    uint32_t pos = 12;
    while (pos + 8 < 44) {
        uint32_t chunk_id = *(uint32_t *)(header + pos);
        uint32_t chunk_size = *(uint32_t *)(header + pos + 4);

        if (chunk_id == *(uint32_t *)"fmt ") {
            *channels = *(uint16_t *)(header + pos + 10);
            *sample_rate = *(uint32_t *)(header + pos + 12);
            *bits_per_sample = *(uint16_t *)(header + pos + 22);
        } else if (chunk_id == *(uint32_t *)"data") {
            *data_size = chunk_size;
            return true;
        }

        pos += 8 + chunk_size;
        if (chunk_size % 2 != 0) pos++;
    }

    return false;
}

static fileplayer_type_t detect_file_type(sdfile_t f) {
    uint8_t header[16];
    memset(header, 0, sizeof(header));

    if (sdcard_fread(f, header, sizeof(header)) < (int)sizeof(header)) {
        return FILEPLAYER_TYPE_UNKNOWN;
    }

    sdcard_fseek(f, 0);

    if (memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0) {
        return FILEPLAYER_TYPE_WAV;
    }

    if (memcmp(header, "ID3", 3) == 0) {
        return FILEPLAYER_TYPE_MP3;
    }

    if ((header[0] == 0xFF && (header[1] & 0xE0) == 0xE0) ||
        (header[0] == 0xFE) || (header[0] == 0xFA) ||
        (header[0] == 0xFB) || (header[0] == 0xFC)) {
        return FILEPLAYER_TYPE_MP3;
    }

    return FILEPLAYER_TYPE_UNKNOWN;
}

void fileplayer_reset(void) {
    if (!s_initialized) return;
    if (s_active_player && s_active_player->state == FILEPLAYER_STATE_PLAYING)
        audio_stop_stream();
    if (s_current_file) {
        sdcard_fclose(s_current_file);
        s_current_file = NULL;
    }
    memset(s_players, 0, sizeof(s_players));
    s_active_player = NULL;
    s_underflow = false;
}

void fileplayer_init(void) {
    if (s_initialized) return;

    printf("[FILEPLAYER] Allocating WAV buffer (%d bytes)...\n", WAV_BUFFER_SIZE);
    s_wav_buffer = umm_malloc(WAV_BUFFER_SIZE);
    printf("[FILEPLAYER] WAV buffer allocated: %s\n", s_wav_buffer ? "OK" : "FAILED");
    if (!s_wav_buffer) return;

    memset(s_players, 0, sizeof(s_players));

    s_initialized = true;
}

fileplayer_t *fileplayer_create(void) {
    for (int i = 0; i < FILEPLAYER_MAX_INSTANCES; i++) {
        if (s_players[i].state == FILEPLAYER_STATE_IDLE) {
            memset(&s_players[i], 0, sizeof(fileplayer_t));
            s_players[i].volume = 100;
            s_players[i].channels = 2;
            s_players[i].rate = 1.0f;
            return &s_players[i];
        }
    }
    return NULL;
}

void fileplayer_destroy(fileplayer_t *player) {
    if (player) {
        fileplayer_stop(player);
        memset(player, 0, sizeof(fileplayer_t));
    }
}

bool fileplayer_load(fileplayer_t *player, const char *path) {
    if (!player || !path) return false;

    if (s_current_file) {
        sdcard_fclose(s_current_file);
        s_current_file = NULL;
    }

    strncpy(player->path, path, sizeof(player->path) - 1);

    s_current_file = sdcard_fopen(path, "rb");
    if (!s_current_file) {
        printf("fileplayer: failed to open %s\n", path);
        return false;
    }

    fileplayer_type_t type = detect_file_type(s_current_file);
    player->type = type;

    if (type == FILEPLAYER_TYPE_MP3) {
        sdcard_fclose(s_current_file);
        s_current_file = NULL;
        printf("fileplayer: MP3 file detected, use sound.mp3player() instead\n");
        return false;
    }

    if (type != FILEPLAYER_TYPE_WAV) {
        sdcard_fclose(s_current_file);
        s_current_file = NULL;
        printf("fileplayer: unknown file format\n");
        return false;
    }

    uint32_t sample_rate = 44100, data_size = 0;
    uint16_t wav_channels = 2, bits = 16;
    if (!parse_wav_header(s_current_file, &sample_rate, &wav_channels, &bits, &data_size)) {
        sdcard_fclose(s_current_file);
        s_current_file = NULL;
        printf("fileplayer: failed to parse WAV\n");
        return false;
    }

    s_sample_rate = sample_rate;
    player->channels = wav_channels;
    player->length = data_size / (wav_channels * bits / 8);
    player->position = 0;

    printf("fileplayer: loaded %s (%lu Hz, %u bit, %u ch, %lu samples)\n",
           path, sample_rate, bits, wav_channels, player->length);

    return true;
}

bool fileplayer_play(fileplayer_t *player, uint8_t repeat_count) {
    (void)repeat_count;
    if (!player || !s_current_file) return false;

    player->state = FILEPLAYER_STATE_PLAYING;
    s_active_player = player;

    // Use the PCM streaming API — its ISR runs on Core 1 via s_core1_alarm_pool,
    // eliminating the 44.1kHz timer ISR that used to preempt Core 0.
    audio_start_stream(s_sample_rate);

    sdcard_fseek(s_current_file, 44);
    player->position = 0;

    return true;
}

void fileplayer_stop(fileplayer_t *player) {
    if (!player) return;

    player->state = FILEPLAYER_STATE_STOPPED;
    player->position = 0;

    if (s_active_player == player) {
        s_active_player = NULL;
    }

    bool any_playing = false;
    for (int i = 0; i < FILEPLAYER_MAX_INSTANCES; i++) {
        if (s_players[i].state == FILEPLAYER_STATE_PLAYING) {
            any_playing = true;
            break;
        }
    }

    if (!any_playing)
        audio_stop_stream();

    if (s_current_file) {
        sdcard_fclose(s_current_file);
        s_current_file = NULL;
    }
}

void fileplayer_pause(fileplayer_t *player) {
    if (!player || player->state != FILEPLAYER_STATE_PLAYING) return;
    player->state = FILEPLAYER_STATE_PAUSED;
}

void fileplayer_resume(fileplayer_t *player) {
    if (!player || player->state != FILEPLAYER_STATE_PAUSED) return;
    player->state = FILEPLAYER_STATE_PLAYING;
}

bool fileplayer_is_playing(const fileplayer_t *player) {
    return player && player->state == FILEPLAYER_STATE_PLAYING;
}

uint32_t fileplayer_get_position(const fileplayer_t *player) {
    if (!player) return 0;
    return player->position / 4;
}

uint32_t fileplayer_get_length(const fileplayer_t *player) {
    if (!player) return 0;
    return player->length;
}

void fileplayer_set_volume(fileplayer_t *player, uint8_t left, uint8_t right) {
    if (!player) return;
    player->volume = left;
    s_volume_l = left;
    s_volume_r = right > 0 ? right : left;
}

void fileplayer_get_volume(const fileplayer_t *player, uint8_t *left, uint8_t *right) {
    if (!player) return;
    *left = player->volume;
    *right = s_volume_r;
}

void fileplayer_set_loop_range(fileplayer_t *player, uint32_t start, uint32_t end) {
    if (!player) return;
    player->loop = true;
    player->loop_start = start;
    player->loop_end = end;
}

void fileplayer_set_finish_callback(fileplayer_t *player, int (*cb)(void *), void *arg) {
    if (!player) return;
    player->finish_callback = cb;
    player->finish_callback_arg = arg;
}

void fileplayer_set_loop_callback(fileplayer_t *player, int (*cb)(void *), void *arg) {
    if (!player) return;
    player->loop_callback = cb;
    player->loop_callback_arg = arg;
}

void fileplayer_set_offset(fileplayer_t *player, uint32_t seconds) {
    if (!player || !s_current_file) return;
    uint32_t offset = seconds * s_sample_rate * 4;
    sdcard_fseek(s_current_file, 44 + offset);
    player->position = offset;
}

uint32_t fileplayer_get_offset(const fileplayer_t *player) {
    if (!player) return 0;
    return player->position / 4 / s_sample_rate;
}

void fileplayer_set_stop_on_underrun(fileplayer_t *player, bool flag) {
    if (!player) return;
    player->stop_on_underrun = flag;
}

void fileplayer_set_rate(fileplayer_t *player, float rate) {
    if (!player) return;
    if (rate < 0.1f) rate = 0.1f;
    if (rate > 4.0f) rate = 4.0f;
    player->rate = rate;
}

float fileplayer_get_rate(const fileplayer_t *player) {
    return player ? player->rate : 1.0f;
}

// Called from Core 1 every 5ms. Reads WAV data from SD, converts to
// stereo int16_t, and pushes into the PCM stream ring buffer.
void fileplayer_update(void) {
    if (!s_initialized) return;
    if (!s_current_file || !s_active_player ||
        s_active_player->state != FILEPLAYER_STATE_PLAYING)
        return;

    // Stop on underrun if configured
    if (s_underflow && s_active_player->stop_on_underrun) {
        fileplayer_stop(s_active_player);
        return;
    }

    // Non-blocking: skip if Core 0 owns the SD card
    if (!recursive_mutex_try_enter(&g_sdcard_mutex, NULL))
        return;

    // Read a chunk of WAV data
    size_t to_read = 4096;
    UINT br = 0;
    FRESULT res = f_read((FIL *)s_current_file, s_wav_buffer, to_read, &br);
    recursive_mutex_exit(&g_sdcard_mutex);

    if (res == FR_OK && br > 0) {
        // WAV data is 16-bit signed PCM. Convert to stereo int16_t pairs
        // and push into the PCM stream ring buffer.
        // Rate resampling: nearest-neighbor. For rate=2.0 we produce half
        // the output frames (pitch up); for rate=0.5 we produce double.
        int16_t *pcm = (int16_t *)s_wav_buffer;
        uint32_t num_samples = br / 2;  // 16-bit samples
        float rate = s_active_player->rate;
        if (rate < 0.1f) rate = 0.1f;

        if (s_active_player->channels == 1) {
            uint32_t in_frames = num_samples;
            uint32_t out_frames = (uint32_t)(in_frames / rate);
            if (out_frames == 0) out_frames = 1;
            int16_t stereo_buf[512];  // 256 stereo frames at a time
            uint32_t pos = 0;
            while (pos < out_frames) {
                uint32_t chunk = out_frames - pos;
                if (chunk > 256) chunk = 256;
                for (uint32_t i = 0; i < chunk; i++) {
                    uint32_t src = (uint32_t)((pos + i) * rate);
                    if (src >= in_frames) src = in_frames - 1;
                    int32_t s = ((int32_t)pcm[src] * s_volume_l) / 100;
                    if (s > 32767) s = 32767;
                    if (s < -32768) s = -32768;
                    stereo_buf[i * 2] = (int16_t)s;
                    stereo_buf[i * 2 + 1] = (int16_t)s;
                }
                audio_push_samples(stereo_buf, chunk);
                pos += chunk;
            }
            s_active_player->position += br;
        } else {
            uint32_t in_frames = num_samples / 2;
            uint32_t out_frames = (uint32_t)(in_frames / rate);
            if (out_frames == 0) out_frames = 1;
            int16_t stereo_buf[512];  // 256 stereo frames at a time
            uint32_t pos = 0;
            while (pos < out_frames) {
                uint32_t chunk = out_frames - pos;
                if (chunk > 256) chunk = 256;
                for (uint32_t i = 0; i < chunk; i++) {
                    uint32_t src = (uint32_t)((pos + i) * rate);
                    if (src >= in_frames) src = in_frames - 1;
                    int32_t l = ((int32_t)pcm[src * 2] * s_volume_l) / 100;
                    int32_t r = ((int32_t)pcm[src * 2 + 1] * s_volume_r) / 100;
                    if (l > 32767) l = 32767;
                    if (l < -32768) l = -32768;
                    if (r > 32767) r = 32767;
                    if (r < -32768) r = -32768;
                    stereo_buf[i * 2] = (int16_t)l;
                    stereo_buf[i * 2 + 1] = (int16_t)r;
                }
                audio_push_samples(stereo_buf, chunk);
                pos += chunk;
            }
            s_active_player->position += br;
        }
    } else if (res != FR_OK || br == 0) {
        if (s_active_player->loop) {
            sdcard_fseek(s_current_file, 44);
            s_active_player->position = 0;
            if (s_active_player->loop_callback)
                s_active_player->loop_callback(s_active_player->loop_callback_arg);
        } else {
            s_active_player->state = FILEPLAYER_STATE_STOPPED;
            if (s_active_player->finish_callback)
                s_active_player->finish_callback(s_active_player->finish_callback_arg);
        }
    }
}

bool fileplayer_did_underrun(void) {
    return s_underflow;
}
