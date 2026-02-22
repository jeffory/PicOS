#include "sound.h"
#include "audio.h"
#include "../hardware.h"
#include "sdcard.h"
#include "hardware/pwm.h"
#include "pico/time.h"
#include "pico/stdlib.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static sound_context_t s_context;
static repeating_timer_t s_playback_timer;
static bool s_timer_active = false;
static uint32_t s_timer_interval_us = 0;

static bool parse_wav_header(sound_sample_t *sample, uint8_t *data, uint32_t size) {
    if (size < 44)
        return false;

    if (memcmp(data, "RIFF", 4) != 0)
        return false;
    if (memcmp(data + 8, "WAVE", 4) != 0)
        return false;

    uint32_t data_offset = 0;
    uint32_t data_size = 0;

    uint32_t pos = 12;
    while (pos + 8 < size) {
        uint32_t chunk_id = *(uint32_t *)(data + pos);
        uint32_t chunk_size = *(uint32_t *)(data + pos + 4);

        if (chunk_id == *(uint32_t *)"fmt ") {
            sample->channels = *(uint16_t *)(data + pos + 10);
            sample->sample_rate = *(uint32_t *)(data + pos + 12);
            sample->bits_per_sample = *(uint16_t *)(data + pos + 22);
        } else if (chunk_id == *(uint32_t *)"data") {
            data_offset = pos + 8;
            data_size = chunk_size;
            break;
        }

        pos += 8 + chunk_size;
        if (chunk_size % 2 != 0)
            pos++;
    }

    if (data_offset == 0 || data_size == 0)
        return false;

    if (data_size > SOUND_MAX_SAMPLE_SIZE)
        data_size = SOUND_MAX_SAMPLE_SIZE;

    sample->data = malloc(data_size);
    if (!sample->data)
        return false;

    memcpy(sample->data, data + data_offset, data_size);
    sample->length = data_size;
    sample->loaded = true;

    return true;
}

void sound_init(void) {
    memset(&s_context, 0, sizeof(s_context));
    s_context.time_offset_us = 0;
}

void sound_update(void) {
    if (!s_timer_active)
        return;

    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        sound_player_t *player = &s_context.players[i];
        if (!player->playing || !player->sample || !player->sample->loaded)
            continue;

        sound_sample_t *sample = player->sample;
        uint32_t pos = player->position;

        if (pos >= sample->length) {
            player->repeats_played++;
            if (player->repeat_count > 0 && player->repeats_played >= player->repeat_count) {
                player->playing = false;
                player->position = 0;
                continue;
            }
            player->position = 0;
            pos = 0;
        }

        uint8_t sample_val = sample->data[pos];
        uint32_t volume = player->volume;
        uint32_t level = (sample_val * volume) / 100;
        level = (level * 128) / 255;

        pwm_set_gpio_level(AUDIO_PIN_L, level);
        pwm_set_gpio_level(AUDIO_PIN_R, level);

        player->position++;
    }

    s_context.time_offset_us += s_timer_interval_us;
}

static bool playback_timer_callback(repeating_timer_t *rt) {
    (void)rt;
    sound_update();
    return true;
}

sound_sample_t *sound_sample_create(void) {
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        if (!s_context.samples[i]) {
            s_context.samples[i] = calloc(1, sizeof(sound_sample_t));
            return s_context.samples[i];
        }
    }
    return NULL;
}

void sound_sample_destroy(sound_sample_t *sample) {
    if (!sample)
        return;
    if (sample->data)
        free(sample->data);
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        if (s_context.samples[i] == sample) {
            s_context.samples[i] = NULL;
            break;
        }
    }
    free(sample);
}

bool sound_sample_load(sound_sample_t *sample, const char *path) {
    if (!sample || !path)
        return false;

    sdfile_t f = sdcard_fopen(path, "rb");
    if (!f) {
        printf("sound: failed to open %s\n", path);
        return false;
    }

    uint8_t header[44];
    int read = sdcard_fread(f, header, sizeof(header));
    if (read < 44) {
        sdcard_fclose(f);
        printf("sound: file too small\n");
        return false;
    }

    uint32_t file_size = sdcard_fsize(path);
    file_size = file_size > SOUND_MAX_SAMPLE_SIZE ? SOUND_MAX_SAMPLE_SIZE : file_size;

    sdcard_fseek(f, 0);
    uint8_t *data = malloc(file_size);
    if (!data) {
        sdcard_fclose(f);
        return false;
    }

    uint32_t bytes_read = sdcard_fread(f, data, file_size);
    sdcard_fclose(f);

    if (!parse_wav_header(sample, data, bytes_read)) {
        free(data);
        printf("sound: failed to parse WAV\n");
        return false;
    }

    free(data);
    printf("sound: loaded %s (%lu Hz, %u bit, %u ch)\n",
           path, sample->sample_rate, sample->bits_per_sample, sample->channels);
    return true;
}

uint32_t sound_sample_get_length(const sound_sample_t *sample) {
    if (!sample || !sample->loaded)
        return 0;
    return sample->length / (sample->channels * sample->bits_per_sample / 8);
}

uint32_t sound_sample_get_sample_rate(const sound_sample_t *sample) {
    if (!sample || !sample->loaded)
        return 0;
    return sample->sample_rate;
}

sound_player_t *sound_player_create(void) {
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        sound_player_t *player = &s_context.players[i];
        if (!player->sample) {
            player->volume = 100;
            return player;
        }
    }
    return NULL;
}

void sound_player_destroy(sound_player_t *player) {
    if (player) {
        sound_player_stop(player);
        player->sample = NULL;
    }
}

bool sound_player_set_sample(sound_player_t *player, sound_sample_t *sample) {
    if (!player || !sample)
        return false;
    player->sample = sample;
    player->position = 0;
    return true;
}

void sound_player_play(sound_player_t *player, uint8_t repeat_count) {
    if (!player || !player->sample || !player->sample->loaded)
        return;

    player->playing = true;
    player->paused = false;
    player->repeat_count = repeat_count;
    player->repeats_played = 0;
    player->position = 0;

    uint32_t sample_rate = player->sample->sample_rate;
    if (sample_rate > 0) {
        s_timer_interval_us = 1000000 / sample_rate;
        if (!s_timer_active) {
            pwm_set_enabled(pwm_gpio_to_slice_num(AUDIO_PIN_L), true);
            pwm_set_enabled(pwm_gpio_to_slice_num(AUDIO_PIN_R), true);
            add_repeating_timer_us(-s_timer_interval_us, playback_timer_callback, NULL, &s_playback_timer);
            s_timer_active = true;
        }
    }
}

void sound_player_stop(sound_player_t *player) {
    if (!player)
        return;
    player->playing = false;
    player->paused = false;
    player->position = 0;
    player->repeat_count = 0;
    player->repeats_played = 0;

    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);

    bool any_playing = false;
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        if (s_context.players[i].playing) {
            any_playing = true;
            break;
        }
    }
    if (!any_playing && s_timer_active) {
        cancel_repeating_timer(&s_playback_timer);
        s_timer_active = false;
        pwm_set_enabled(pwm_gpio_to_slice_num(AUDIO_PIN_L), false);
        pwm_set_enabled(pwm_gpio_to_slice_num(AUDIO_PIN_R), false);
    }
}

void sound_player_set_volume(sound_player_t *player, uint8_t volume) {
    if (!player)
        return;
    if (volume > 100)
        volume = 100;
    player->volume = volume;
}

bool sound_player_is_playing(const sound_player_t *player) {
    return player && player->playing;
}

uint32_t sound_get_current_time(void) {
    return s_context.time_offset_us / 1000000;
}

void sound_reset_time(void) {
    s_context.time_offset_us = 0;
}
