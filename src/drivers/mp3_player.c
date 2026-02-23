#include "mp3_player.h"
#include "../hardware.h"
#include "sdcard.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "pico/time.h"

#include "mp3dec.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MP3_DECODE_BUFFER_SIZE 8192

static HMP3Decoder s_decoder = NULL;
static sdfile_t s_file = NULL;
static uint8_t s_decode_buffer[MP3_DECODE_BUFFER_SIZE];
static int s_bytes_in_buffer = 0;
static int s_buffer_pos = 0;

static mp3_player_t s_player;
static bool s_initialized = false;
static repeating_timer_t s_playback_timer;
static bool s_timer_active = false;

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

    pwm_set_gpio_level(AUDIO_PIN_L, 128);
    pwm_set_gpio_level(AUDIO_PIN_R, 128);
}

static bool is_mp3(const uint8_t *header) {
    if (header[0] == 0xFF && (header[1] & 0xE0) == 0xE0) {
        return true;
    }
    if (memcmp(header, "ID3", 3) == 0) {
        return true;
    }
    return false;
}

static int sync_to_frame(sdfile_t f, uint8_t *buffer, int *bytes_in_buffer) {
    int pos = 0;
    
    while (*bytes_in_buffer < 4) {
        int read = sdcard_fread(f, buffer + *bytes_in_buffer, 4096 - *bytes_in_buffer);
        if (read <= 0) return -1;
        *bytes_in_buffer += read;
    }
    
    while (pos < *bytes_in_buffer - 4) {
        if (buffer[pos] == 0xFF && (buffer[pos + 1] & 0xE0) == 0xE0) {
            return pos;
        }
        pos++;
    }
    
    memmove(buffer, buffer + pos, *bytes_in_buffer - pos);
    *bytes_in_buffer -= pos;
    
    int read = sdcard_fread(f, buffer + *bytes_in_buffer, 4096 - *bytes_in_buffer);
    if (read > 0) *bytes_in_buffer += read;
    
    return sync_to_frame(f, buffer, bytes_in_buffer);
}

static bool playback_callback(repeating_timer_t *rt) {
    (void)rt;

    if (!s_player.playing || s_player.paused || !s_decoder) {
        return true;
    }

    short pcm_output[1152 * 2];
    unsigned char *inbuf = s_decode_buffer + s_buffer_pos;
    int err = MP3Decode(s_decoder, &inbuf, &s_bytes_in_buffer, pcm_output, 0);

    if (err == ERR_MP3_INDATA_UNDERFLOW) {
        int read = sdcard_fread(s_file, s_decode_buffer, sizeof(s_decode_buffer));
        if (read > 0) {
            s_bytes_in_buffer = read;
            s_buffer_pos = 0;
        } else {
            if (s_player.loop) {
                sdcard_fseek(s_file, 0);
                s_bytes_in_buffer = 0;
                s_buffer_pos = 0;
                read = sdcard_fread(s_file, s_decode_buffer, sizeof(s_decode_buffer));
                if (read > 0) s_bytes_in_buffer = read;
            } else {
                s_player.playing = false;
                pwm_set_gpio_level(AUDIO_PIN_L, 0);
                pwm_set_gpio_level(AUDIO_PIN_R, 0);
                if (s_timer_active) {
                    cancel_repeating_timer(&s_playback_timer);
                    s_timer_active = false;
                }
            }
        }
        return true;
    } else if (err == 0) {
        MP3FrameInfo info;
        MP3GetLastFrameInfo(s_decoder, &info);
        
        int16_t left = pcm_output[0];
        int16_t right = (info.nChans > 1) ? pcm_output[1] : left;

        int32_t left_val = ((int32_t)left + 32768) * s_player.volume / 100;
        int32_t right_val = ((int32_t)right + 32768) * s_player.volume / 100;

        left_val = ((uint64_t)left_val * 128) / 255;
        right_val = ((uint64_t)right_val * 128) / 255;
        
        if (left_val > 255) left_val = 255;
        if (right_val > 255) right_val = 255;

        pwm_set_gpio_level(AUDIO_PIN_L, (uint8_t)left_val);
        pwm_set_gpio_level(AUDIO_PIN_R, (uint8_t)right_val);
        
        s_player.position += info.outputSamps;
    }

    return true;
}

bool mp3_player_init(void) {
    if (s_initialized) return true;
    
    s_decoder = MP3InitDecoder();
    if (!s_decoder) {
        printf("mp3_player: failed to init decoder\n");
        return false;
    }
    
    memset(&s_player, 0, sizeof(s_player));
    s_player.volume = 100;
    s_initialized = true;
    
    return true;
}

mp3_player_t *mp3_player_create(void) {
    if (!s_initialized) {
        mp3_player_init();
    }
    return &s_player;
}

void mp3_player_destroy(mp3_player_t *player) {
    (void)player;
    mp3_player_stop(player);
}

bool mp3_player_load(mp3_player_t *player, const char *path) {
    if (!player || !path) return false;

    if (s_timer_active) {
        cancel_repeating_timer(&s_playback_timer);
        s_timer_active = false;
    }

    if (s_file) {
        sdcard_fclose(s_file);
        s_file = NULL;
    }

    s_file = sdcard_fopen(path, "rb");
    if (!s_file) {
        printf("mp3_player: failed to open %s\n", path);
        return false;
    }

    int read = sdcard_fread(s_file, s_decode_buffer, sizeof(s_decode_buffer));
    if (read <= 0) {
        sdcard_fclose(s_file);
        s_file = NULL;
        return false;
    }
    s_bytes_in_buffer = read;
    s_buffer_pos = 0;

    if (s_bytes_in_buffer > 0) {
        MP3FrameInfo info;
        int err = MP3GetNextFrameInfo(s_decoder, &info, s_decode_buffer);
        if (err < 0) {
            printf("mp3_player: not an MP3 file\n");
            sdcard_fclose(s_file);
            s_file = NULL;
            return false;
        }
        
        player->sample_rate = info.samprate;
        player->channels = info.nChans;
        player->length = 0;
        player->position = 0;
        
        printf("mp3_player: loaded %s (%d Hz, %d ch)\n", path, info.samprate, info.nChans);
    }

    return true;
}

bool mp3_player_play(mp3_player_t *player, uint8_t repeat_count) {
    (void)repeat_count;
    if (!player || !s_file) return false;

    player->playing = true;
    player->paused = false;

    pwm_stereo_init(player->sample_rate);

    if (!s_timer_active) {
        int interval_us = 1000000 / player->sample_rate / 4;
        add_repeating_timer_us(-interval_us, playback_callback, NULL, &s_playback_timer);
        s_timer_active = true;
    }

    return true;
}

void mp3_player_stop(mp3_player_t *player) {
    if (!player) return;

    player->playing = false;
    player->paused = false;
    player->position = 0;

    if (s_timer_active) {
        cancel_repeating_timer(&s_playback_timer);
        s_timer_active = false;
    }

    if (s_file) {
        sdcard_fclose(s_file);
        s_file = NULL;
    }

    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;

    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);
}

void mp3_player_pause(mp3_player_t *player) {
    if (!player) return;
    player->paused = true;
}

void mp3_player_resume(mp3_player_t *player) {
    if (!player) return;
    player->paused = false;
}

bool mp3_player_is_playing(const mp3_player_t *player) {
    return player && player->playing && !player->paused;
}

uint32_t mp3_player_get_position(const mp3_player_t *player) {
    if (!player) return 0;
    return player->position / player->channels;
}

uint32_t mp3_player_get_length(const mp3_player_t *player) {
    if (!player) return 0;
    return player->length;
}

void mp3_player_set_volume(mp3_player_t *player, uint8_t volume) {
    if (!player) return;
    if (volume > 100) volume = 100;
    player->volume = volume;
}

uint8_t mp3_player_get_volume(const mp3_player_t *player) {
    if (!player) return 0;
    return player->volume;
}

void mp3_player_set_loop(mp3_player_t *player, bool loop) {
    if (!player) return;
    player->loop = loop;
}

void mp3_player_update(void) {
}
