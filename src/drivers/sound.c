#include "sound.h"
#include "audio.h"
#include "../hardware.h"
#include "sdcard.h"
#include "pico/time.h"
#include "pico/stdlib.h"
#include "umm_malloc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static sound_context_t s_context;

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

        if (chunk_size > size - pos - 8)
            break; // malformed chunk — would read past buffer

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

    sample->data = umm_malloc(data_size);
    if (!sample->data)
        return false;

    memcpy(sample->data, data + data_offset, data_size);
    sample->length = data_size;
    sample->loaded = true;

    return true;
}

void sound_init(void) {
    // Reclaim any loaded sample data before dropping the pointers (app exit
    // must not leak PSRAM, even for path-constructed samples).
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        sound_sample_t *s = s_context.samples[i];
        if (s) {
            if (s->data) umm_free(s->data);
            free(s);
            s_context.samples[i] = NULL;
        }
    }
    memset(&s_context, 0, sizeof(s_context));
}

/* Mixer entry point: called from audio.c's DMA refill hook (Core 1 ISR) to
 * produce `frames` frames of mixed sample audio at AUDIO_OUT_RATE. Each
 * active player advances through its sample data via a phase accumulator
 * (nearest-neighbor rate conversion). Accumulates into out_l/out_r (int32)
 * — the caller clips to int16 when mixing with other sources. */
void sound_mixer_process(int32_t *out_l, int32_t *out_r, int frames) {
    memset(out_l, 0, frames * sizeof(*out_l));
    memset(out_r, 0, frames * sizeof(*out_r));

    for (int p = 0; p < SOUND_MAX_SAMPLES; p++) {
        sound_player_t *player = &s_context.players[p];
        if (!player->playing || player->paused || !player->sample || !player->sample->loaded)
            continue;

        sound_sample_t *sample = player->sample;
        uint32_t bpf = (sample->bits_per_sample / 8) * sample->channels;

        uint32_t effective_end = sample->length;
        if (player->play_end > 0) {
            uint32_t end_bytes = player->play_end * bpf;
            if (end_bytes < effective_end)
                effective_end = end_bytes;
        }
        uint32_t effective_start = player->play_start > 0 ? player->play_start * bpf : 0;
        if (player->position < effective_start)
            player->position = effective_start;

        uint32_t step = (uint32_t)(sample->sample_rate * player->rate);

        for (int i = 0; i < frames; i++) {
            if (!player->playing)
                break;
            uint32_t pos = player->position;
            if (pos >= effective_end) {
                player->repeats_played++;
                if (player->repeat_count > 0 && player->repeats_played >= player->repeat_count) {
                    player->playing = false;
                    player->position = effective_start;
                    player->finish_pending = true;
                    break;
                }
                player->loop_pending = true;
                player->position = effective_start;
                pos = effective_start;
            }

            int16_t l16, r16;
            if (sample->bits_per_sample == 16) {
                l16 = *(int16_t *)(sample->data + pos);
                r16 = sample->channels >= 2 ? *(int16_t *)(sample->data + pos + 2) : l16;
            } else {
                l16 = ((int16_t)sample->data[pos] - 128) << 8;
                r16 = sample->channels >= 2 ? ((int16_t)sample->data[pos + 1] - 128) << 8 : l16;
            }
            out_l[i] += (l16 * player->volume) / 100;
            out_r[i] += (r16 * player->volume) / 100;

            player->phase += step;
            while (player->phase >= AUDIO_OUT_RATE) {
                player->phase -= AUDIO_OUT_RATE;
                player->position += bpf;
            }
        }
    }
    // Keep the public time base advancing (~us of audio mixed)
    s_context.time_offset_us += (uint32_t)(((uint64_t)frames * 1000000) / AUDIO_OUT_RATE);
}

/* Fires deferred finish/loop callbacks — call from the Core 1 work pump
 * (NOT from the mixer ISR; callbacks trampoline into Lua). */
void sound_pump_callbacks(void) {
    for (int p = 0; p < SOUND_MAX_SAMPLES; p++) {
        sound_player_t *player = &s_context.players[p];
        if (player->finish_pending) {
            player->finish_pending = false;
            if (player->finish_callback)
                player->finish_callback(player->finish_callback_arg);
        }
        if (player->loop_pending) {
            player->loop_pending = false;
            if (player->loop_callback)
                player->loop_callback(player->loop_callback_arg);
        }
    }
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
        umm_free(sample->data);
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        if (s_context.samples[i] == sample) {
            s_context.samples[i] = NULL;
            break;
        }
    }
    free(sample);  // allocated with calloc(), not umm_malloc
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
    uint8_t *data = umm_malloc(file_size);
    if (!data) {
        sdcard_fclose(f);
        return false;
    }

    uint32_t bytes_read = sdcard_fread(f, data, file_size);
    sdcard_fclose(f);

    if (!parse_wav_header(sample, data, bytes_read)) {
        umm_free(data);
        printf("sound: failed to parse WAV\n");
        return false;
    }

    umm_free(data);
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
            player->play_start = 0;
            player->play_end = 0;
            player->rate = 1.0f;
            player->owns_sample = false;
            player->phase = 0;
            player->finish_pending = false;
            player->loop_pending = false;
            player->finish_callback = NULL;
            player->finish_callback_arg = NULL;
            player->loop_callback = NULL;
            player->loop_callback_arg = NULL;
            return player;
        }
    }
    return NULL;
}

void sound_player_destroy(sound_player_t *player) {
    if (player) {
        sound_player_stop(player);
        if (player->owns_sample && player->sample) {
            sound_sample_destroy(player->sample);
        }
        player->sample = NULL;
        player->owns_sample = false;
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
    player->phase = 0;
    player->finish_pending = false;
    player->loop_pending = false;

    // Samples are mixed into the PCM stream by audio.c's DMA refill hook —
    // the stream must be running. No PWM re-init, no playback timer.
    audio_stream_ensure_running();
}

void sound_player_stop(sound_player_t *player) {
    if (!player)
        return;
    player->playing = false;
    player->paused = false;
    player->position = 0;
    player->repeat_count = 0;
    player->repeats_played = 0;
}

void sound_player_set_volume(sound_player_t *player, uint8_t volume) {
    if (!player)
        return;
    if (volume > 100)
        volume = 100;
    player->volume = volume;
}

uint8_t sound_player_get_volume(const sound_player_t *player) {
    return player ? player->volume : 0;
}

bool sound_player_is_playing(const sound_player_t *player) {
    return player && player->playing;
}

void sound_player_set_play_range(sound_player_t *player, uint32_t start, uint32_t end) {
    if (!player) return;
    player->play_start = start;
    player->play_end = end;
}

void sound_player_set_rate(sound_player_t *player, float rate) {
    if (!player) return;
    if (rate < 0.0f) rate = 0.0f;
    player->rate = rate;
}

float sound_player_get_rate(const sound_player_t *player) {
    return player ? player->rate : 1.0f;
}

void sound_player_set_finish_callback(sound_player_t *player, int (*cb)(void *), void *arg) {
    if (!player) return;
    player->finish_callback = cb;
    player->finish_callback_arg = arg;
}

void sound_player_set_loop_callback(sound_player_t *player, int (*cb)(void *), void *arg) {
    if (!player) return;
    player->loop_callback = cb;
    player->loop_callback_arg = arg;
}

sound_sample_t *sound_sample_new_blank(float seconds, uint32_t sample_rate, uint8_t bits_per_sample, uint8_t channels) {
    sound_sample_t *sample = sound_sample_create();
    if (!sample) return NULL;

    uint32_t bytes_per_frame = (bits_per_sample / 8) * channels;
    uint32_t num_frames = (uint32_t)(seconds * sample_rate);
    uint32_t data_size = num_frames * bytes_per_frame;

    if (data_size > SOUND_MAX_SAMPLE_SIZE)
        data_size = SOUND_MAX_SAMPLE_SIZE;

    sample->data = umm_malloc(data_size);
    if (!sample->data) {
        sound_sample_destroy(sample);
        return NULL;
    }

    memset(sample->data, 0, data_size);
    sample->length = data_size;
    sample->sample_rate = sample_rate;
    sample->bits_per_sample = bits_per_sample;
    sample->channels = channels;
    sample->loaded = true;

    return sample;
}

sound_sample_t *sound_sample_get_subsample(const sound_sample_t *sample, uint32_t start_frame, uint32_t end_frame) {
    if (!sample || !sample->loaded || !sample->data)
        return NULL;

    uint32_t bytes_per_frame = (sample->bits_per_sample / 8) * sample->channels;
    uint32_t total_frames = sample->length / bytes_per_frame;

    if (start_frame >= total_frames) start_frame = total_frames;
    if (end_frame > total_frames) end_frame = total_frames;
    if (end_frame <= start_frame) return NULL;

    uint32_t num_frames = end_frame - start_frame;
    uint32_t data_size = num_frames * bytes_per_frame;

    sound_sample_t *sub = sound_sample_create();
    if (!sub) return NULL;

    sub->data = umm_malloc(data_size);
    if (!sub->data) {
        sound_sample_destroy(sub);
        return NULL;
    }

    memcpy(sub->data, sample->data + start_frame * bytes_per_frame, data_size);
    sub->length = data_size;
    sub->sample_rate = sample->sample_rate;
    sub->bits_per_sample = sample->bits_per_sample;
    sub->channels = sample->channels;
    sub->loaded = true;

    return sub;
}

int sound_get_playing_source_count(void) {
    int count = 0;
    for (int i = 0; i < SOUND_MAX_SAMPLES; i++) {
        if (s_context.players[i].playing)
            count++;
    }
    return count;
}

uint32_t sound_get_current_time(void) {
    return s_context.time_offset_us / 1000000;
}

void sound_reset_time(void) {
    s_context.time_offset_us = 0;
}
