#include "fileplayer.h"
#include "../hardware.h"
#include "sdcard.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "mp3_player.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define WAV_BUFFER_SIZE FILEPLAYER_BUFFER_SIZE

typedef struct {
    uint8_t *buffer;
    volatile size_t read_pos;
    volatile size_t write_pos;
    size_t size;
    volatile bool underflow;
} ring_buffer_t;

static ring_buffer_t s_ring_buffer;
static fileplayer_t s_players[FILEPLAYER_MAX_INSTANCES];
static fileplayer_t *s_active_player = NULL;
static repeating_timer_t s_playback_timer;
static bool s_timer_active = false;
static uint32_t s_sample_rate = 44100;
static uint8_t s_volume_l = 100;
static uint8_t s_volume_r = 100;
static bool s_initialized = false;
static sdfile_t s_current_file = NULL;
static uint8_t *s_wav_buffer = NULL;

static void pwm_stereo_init(uint32_t sample_rate) {
    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);

    uint slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
    uint slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, 255);

    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t div = sys_clk / (sample_rate * 256);
    if (div < 1) div = 1;
    if (div > 255) div = 255;
    pwm_config_set_clkdiv(&cfg, div);

    pwm_init(slice_l, &cfg, true);
    pwm_init(slice_r, &cfg, true);

    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);
}

static void ring_buffer_init(ring_buffer_t *rb, size_t size) {
    rb->buffer = malloc(size);
    rb->size = size;
    rb->read_pos = 0;
    rb->write_pos = 0;
    rb->underflow = false;
}

static size_t ring_buffer_write(ring_buffer_t *rb, const uint8_t *data, size_t len) {
    size_t written = 0;
    for (size_t i = 0; i < len; i++) {
        size_t next = (rb->write_pos + 1) % rb->size;
        if (next == rb->read_pos) {
            break;
        }
        rb->buffer[rb->write_pos] = data[i];
        rb->write_pos = next;
        written++;
    }
    return written;
}

static size_t ring_buffer_read(ring_buffer_t *rb, uint8_t *data, size_t len) {
    size_t available = (rb->write_pos >= rb->read_pos) ?
        (rb->write_pos - rb->read_pos) :
        (rb->size - rb->read_pos + rb->write_pos);
    
    if (available == 0) {
        rb->underflow = true;
        return 0;
    }

    size_t to_read = (len < available) ? len : available;
    size_t first = (rb->size - rb->read_pos);
    if (first >= to_read) {
        memcpy(data, rb->buffer + rb->read_pos, to_read);
        rb->read_pos = (rb->read_pos + to_read) % rb->size;
    } else {
        memcpy(data, rb->buffer + rb->read_pos, first);
        memcpy(data + first, rb->buffer, to_read - first);
        rb->read_pos = to_read - first;
    }

    if (available > 0) {
        rb->underflow = false;
    }

    return to_read;
}

static size_t ring_buffer_available(ring_buffer_t *rb) {
    if (rb->write_pos >= rb->read_pos) {
        return rb->write_pos - rb->read_pos;
    }
    return rb->size - rb->read_pos + rb->write_pos;
}

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

static bool fill_buffer_callback(repeating_timer_t *rt) {
    (void)rt;

    if (!s_current_file || !s_active_player || s_active_player->state != FILEPLAYER_STATE_PLAYING) {
        return true;
    }

    size_t space = s_ring_buffer.size - ring_buffer_available(&s_ring_buffer);
    if (space > 256) {
        int bytes_read = sdcard_fread(s_current_file, s_wav_buffer, space);
        if (bytes_read > 0) {
            ring_buffer_write(&s_ring_buffer, s_wav_buffer, bytes_read);
        } else {
            if (s_active_player->loop) {
                sdcard_fseek(s_current_file, 44);
                s_active_player->position = 0;
            } else {
                s_active_player->state = FILEPLAYER_STATE_STOPPED;
                if (s_active_player->finish_callback) {
                    s_active_player->finish_callback(s_active_player->finish_callback_arg);
                }
            }
        }
    }

    return true;
}

static bool playback_callback(repeating_timer_t *rt) {
    (void)rt;

    if (!s_active_player || s_active_player->state != FILEPLAYER_STATE_PLAYING) {
        return true;
    }

    size_t bytes_per_sample = 2 * 2;
    size_t to_read = bytes_per_sample;

    if (ring_buffer_available(&s_ring_buffer) < to_read) {
        return true;
    }

    uint8_t sample_buf[4];
    if (ring_buffer_read(&s_ring_buffer, sample_buf, to_read) < to_read) {
        return true;
    }

    int16_t left = *(int16_t *)sample_buf;
    int16_t right = (s_active_player->state == FILEPLAYER_STATE_PLAYING) ?
        *(int16_t *)(sample_buf + 2) : left;

    int32_t left_val = ((int32_t)left + 32768) * s_volume_l / 100;
    int32_t right_val = ((int32_t)right + 32768) * s_volume_r / 100;

    left_val = (left_val * 128) / 255;
    right_val = (right_val * 128) / 255;

    pwm_set_gpio_level(AUDIO_PIN_L, left_val);
    pwm_set_gpio_level(AUDIO_PIN_R, right_val);

    s_active_player->position += to_read;

    return true;
}

void fileplayer_init(void) {
    if (s_initialized) return;

    ring_buffer_init(&s_ring_buffer, WAV_BUFFER_SIZE);
    s_wav_buffer = malloc(WAV_BUFFER_SIZE);

    memset(s_players, 0, sizeof(s_players));

    s_initialized = true;
}

fileplayer_t *fileplayer_create(void) {
    for (int i = 0; i < FILEPLAYER_MAX_INSTANCES; i++) {
        if (s_players[i].state == FILEPLAYER_STATE_IDLE) {
            memset(&s_players[i], 0, sizeof(fileplayer_t));
            s_players[i].volume = 100;
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
    uint16_t channels = 2, bits = 16;
    if (!parse_wav_header(s_current_file, &sample_rate, &channels, &bits, &data_size)) {
        sdcard_fclose(s_current_file);
        s_current_file = NULL;
        printf("fileplayer: failed to parse WAV\n");
        return false;
    }

    s_sample_rate = sample_rate;
    player->length = data_size / (channels * bits / 8);
    player->position = 0;

    printf("fileplayer: loaded %s (%lu Hz, %u bit, %u ch, %lu samples)\n",
           path, sample_rate, bits, channels, player->length);

    return true;
}

bool fileplayer_play(fileplayer_t *player, uint8_t repeat_count) {
    (void)repeat_count;
    if (!player || !s_current_file) return false;

    player->state = FILEPLAYER_STATE_PLAYING;
    s_active_player = player;

    pwm_stereo_init(s_sample_rate);

    if (!s_timer_active) {
        int interval_us = 1000000 / s_sample_rate;
        add_repeating_timer_us(-interval_us, playback_callback, NULL, &s_playback_timer);
        add_repeating_timer_us(-10000, fill_buffer_callback, NULL, NULL);
        s_timer_active = true;
    }

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

    if (s_current_file) {
        sdcard_fclose(s_current_file);
        s_current_file = NULL;
    }

    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);
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

void fileplayer_update(void) {
}

bool fileplayer_did_underrun(void) {
    return s_ring_buffer.underflow;
}
