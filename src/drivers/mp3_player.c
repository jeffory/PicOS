#include "mp3_player.h"
#include "audio.h"
#include "../hardware.h"
#include "sdcard.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "pico/mutex.h"
#include "pico/time.h"

#define FPM_DEFAULT
#include "mad.h"

#include "umm_malloc.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MP3_DECODE_BUFFER_SIZE (8192 + MAD_BUFFER_GUARD)
#define PCM_RING_SIZE          16384  // bytes — ~4096 stereo samples (~170ms @ 24kHz)
#define DMA_BUF_SAMPLES        256   // samples per DMA buffer (~10.7ms @ 24kHz)

#define PWM_WRAP  2047
#define PWM_MID   (PWM_WRAP / 2)     // 1023 — true silence for AC-coupled output

#define FADE_SAMPLES 64              // ~1.5ms at 44.1kHz — inaudible transition

typedef enum {
    FADE_NONE,
    FADE_IN,
    FADE_OUT,
} fade_state_t;

static volatile fade_state_t s_fade_state = FADE_NONE;
static volatile int          s_fade_pos   = 0;
static volatile bool         s_stop_after_fade = false;

// ── Compressed-data state ───────────────────────────────────────────────────
static struct mad_stream *s_mad_stream = NULL;
static struct mad_frame  *s_mad_frame  = NULL;
static struct mad_synth  *s_mad_synth  = NULL;
static sdfile_t    s_file = NULL;
static uint8_t     s_decode_buffer[MP3_DECODE_BUFFER_SIZE] __attribute__((aligned(4)));
static int         s_bytes_in_buffer = 0;
static int         s_buffer_pos = 0;

// ── Player state ────────────────────────────────────────────────────────────
static mp3_player_t s_player;
static bool         s_initialized = false;
static mutex_t      s_mp3_mutex;   // protects decode state from Core 0/1 races

// ── PCM ring buffer (PSRAM — written by decode, read by DMA ISR) ────────────
static uint8_t *s_pcm_ring = NULL;
static volatile size_t s_ring_rd = 0;
static volatile size_t s_ring_wr = 0;

static inline size_t ring_available(void) {
    size_t wr = s_ring_wr, rd = s_ring_rd;
    return (wr >= rd) ? (wr - rd) : (PCM_RING_SIZE - rd + wr);
}

static inline size_t ring_free(void) {
    return PCM_RING_SIZE - 1 - ring_available();
}

static void ring_write(const uint8_t *data, size_t len) {
    while (len > 0) {
        size_t wr = s_ring_wr;
        size_t free_space = ring_free();
        if (free_space == 0) break;

        size_t chunk = (len < free_space) ? len : free_space;
        size_t to_end = PCM_RING_SIZE - wr;
        if (chunk <= to_end) {
            memcpy(s_pcm_ring + wr, data, chunk);
        } else {
            memcpy(s_pcm_ring, data + to_end, chunk - to_end);
            memcpy(s_pcm_ring + wr, data, to_end);
        }
        s_ring_wr = (wr + chunk) % PCM_RING_SIZE;
        data += chunk;
        len -= chunk;
    }
}

// ── DMA audio output (paced by PWM DREQ — zero jitter) ─────────────────────
static int  s_dma_chan = -1;
static uint32_t s_dma_buf[2][DMA_BUF_SAMPLES];  // ping-pong SRAM buffers
static volatile int s_dma_active_buf = 0;
static bool s_dma_active = false;
static int  s_pcm_channels = 2;
static unsigned int s_pwm_slice = 0;

// ── Fill one DMA buffer from the PCM ring (called from DMA ISR) ─────────────
static void fill_dma_buffer(uint32_t *buf, int count) {
    size_t bytes_per_pair = (s_pcm_channels > 1) ? 4 : 2;
    uint32_t vol = s_player.volume;

    for (int i = 0; i < count; i++) {
        int32_t lv, rv;

        if (ring_available() < bytes_per_pair) {
            lv = PWM_MID;
            rv = PWM_MID;
        } else {
            uint8_t raw[4];
            size_t rd = s_ring_rd;
            for (size_t j = 0; j < bytes_per_pair; j++) {
                raw[j] = s_pcm_ring[rd];
                rd = (rd + 1) % PCM_RING_SIZE;
            }
            s_ring_rd = rd;

            int16_t left, right;
            memcpy(&left, raw, 2);
            if (s_pcm_channels > 1)
                memcpy(&right, raw + 2, 2);
            else
                right = left;

            // 16-bit signed → 11-bit unsigned (0–2047)
            lv = (int32_t)((left  + 32768) >> 5);
            rv = (int32_t)((right + 32768) >> 5);
            lv = lv * vol / 100;
            rv = rv * vol / 100;
            if (lv > PWM_WRAP) lv = PWM_WRAP;
            if (rv > PWM_WRAP) rv = PWM_WRAP;
        }

        // Apply fade envelope
        if (s_fade_state == FADE_IN) {
            int32_t gain = (s_fade_pos * 256) / FADE_SAMPLES;
            lv = PWM_MID + (((lv - PWM_MID) * gain) >> 8);
            rv = PWM_MID + (((rv - PWM_MID) * gain) >> 8);
            if (++s_fade_pos >= FADE_SAMPLES)
                s_fade_state = FADE_NONE;
        } else if (s_fade_state == FADE_OUT) {
            int32_t gain = ((FADE_SAMPLES - s_fade_pos) * 256) / FADE_SAMPLES;
            lv = PWM_MID + (((lv - PWM_MID) * gain) >> 8);
            rv = PWM_MID + (((rv - PWM_MID) * gain) >> 8);
            if (++s_fade_pos >= FADE_SAMPLES) {
                s_fade_state = FADE_NONE;
                if (s_stop_after_fade)
                    s_player.playing = false;
            }
        }

        buf[i] = ((uint32_t)rv << 16) | (uint32_t)lv;
    }
}

// ── DMA completion ISR: swap buffers, restart, refill ───────────────────────
static void dma_audio_irq_handler(void) {
    dma_hw->ints1 = 1u << s_dma_chan;          // clear IRQ (using DMA_IRQ_1)

    if (!s_player.playing) {
        // Stop: output silence at midpoint and don't restart
        pwm_set_gpio_level(AUDIO_PIN_L, PWM_MID);
        pwm_set_gpio_level(AUDIO_PIN_R, PWM_MID);
        s_dma_active = false;
        return;
    }

    // Swap to the pre-filled buffer and start DMA immediately
    int next_buf = s_dma_active_buf ^ 1;
    dma_channel_set_read_addr(s_dma_chan, s_dma_buf[next_buf], true);

    // If paused, output silence but keep DMA running (preserves ring buffer)
    if (s_player.paused) {
        for (int i = 0; i < DMA_BUF_SAMPLES; i++)
            s_dma_buf[s_dma_active_buf][i] = ((uint32_t)PWM_MID << 16) | PWM_MID;
        s_dma_active_buf = next_buf;
        return;
    }

    // Refill the buffer that just finished playing
    fill_dma_buffer(s_dma_buf[s_dma_active_buf], DMA_BUF_SAMPLES);
    s_player.position += DMA_BUF_SAMPLES;
    s_dma_active_buf = next_buf;
}

// ── Refill compressed-data buffer from SD card ──────────────────────────────
static bool refill_decode_buffer(void) {
    if (!s_file) return false;

    // Shift leftover data to front
    if (s_buffer_pos > 0 && s_bytes_in_buffer > 0) {
        memmove(s_decode_buffer, s_decode_buffer + s_buffer_pos, s_bytes_in_buffer);
    }
    s_buffer_pos = 0;

    int space = (int)MP3_DECODE_BUFFER_SIZE - s_bytes_in_buffer - MAD_BUFFER_GUARD;
    if (space > 0) {
        int rd = sdcard_fread(s_file, s_decode_buffer + s_bytes_in_buffer, space);
        if (rd > 0)
            s_bytes_in_buffer += rd;
    }

    // Zero-pad guard bytes for libmad
    memset(s_decode_buffer + s_bytes_in_buffer, 0, MAD_BUFFER_GUARD);

    return s_bytes_in_buffer > 0;
}

// ── Decode: fill PCM ring buffer (called from main loop, NOT ISR) ───────────
static void decode_fill_ring(void) {
    if (!s_player.playing || s_player.paused || !s_mad_stream || !s_file)
        return;

    // Decode frames as long as the ring buffer has room for a full frame
    while (ring_free() >= 1152 * 2 * 2) {
        mad_stream_buffer(s_mad_stream, s_decode_buffer + s_buffer_pos, s_bytes_in_buffer + MAD_BUFFER_GUARD);

        if (mad_frame_decode(s_mad_frame, s_mad_stream) != 0) {
            // Track consumed bytes
            if (s_mad_stream->next_frame) {
                int consumed = (int)(s_mad_stream->next_frame - (s_decode_buffer + s_buffer_pos));
                if (consumed > 0 && consumed <= s_bytes_in_buffer) {
                    s_buffer_pos += consumed;
                    s_bytes_in_buffer -= consumed;
                }
            }

            if (s_mad_stream->error == MAD_ERROR_BUFLEN) {
                if (!refill_decode_buffer()) {
                    if (s_player.loop) {
                        sdcard_fseek(s_file, 0);
                        s_bytes_in_buffer = 0;
                        s_buffer_pos = 0;
                        refill_decode_buffer();
                        mad_stream_init(s_mad_stream);
                        mad_frame_init(s_mad_frame);
                        mad_synth_init(s_mad_synth);
                        continue;
                    }
                    s_player.playing = false;
                    break;
                }
                continue;
            }

            if (MAD_RECOVERABLE(s_mad_stream->error)) {
                continue;
            }

            // Non-recoverable error — stop
            printf("[MP3] libmad error: 0x%04x\n", s_mad_stream->error);
            s_player.playing = false;
            break;
        }

        // Track consumed bytes on success
        if (s_mad_stream->next_frame) {
            int consumed = (int)(s_mad_stream->next_frame - (s_decode_buffer + s_buffer_pos));
            if (consumed > 0 && consumed <= s_bytes_in_buffer) {
                s_buffer_pos += consumed;
                s_bytes_in_buffer -= consumed;
            }
        }

        mad_synth_frame(s_mad_synth, s_mad_frame);

        struct mad_pcm *pcm = &s_mad_synth->pcm;
        s_pcm_channels = pcm->channels;
        unsigned int nsamples = pcm->length;

        if (pcm->channels == 2) {
            // Stereo: samplesX is already interleaved [sample][2] int16_t
            ring_write((const uint8_t *)pcm->samplesX, nsamples * 2 * sizeof(int16_t));
        } else {
            // Mono: write only left channel (samplesX[n][0])
            for (unsigned int i = 0; i < nsamples; i++) {
                ring_write((const uint8_t *)&pcm->samplesX[i][0], sizeof(int16_t));
            }
        }
    }
}

// ── Stop DMA and PWM ────────────────────────────────────────────────────────
static void stop_playback(void) {
    if (s_dma_active && s_dma_chan >= 0) {
        dma_channel_abort(s_dma_chan);
        s_dma_active = false;
    }
    // Midpoint is true silence for AC-coupled output (no pop)
    pwm_set_gpio_level(AUDIO_PIN_L, PWM_MID);
    pwm_set_gpio_level(AUDIO_PIN_R, PWM_MID);
    s_fade_state = FADE_NONE;
    s_stop_after_fade = false;
}

// ── Public API ──────────────────────────────────────────────────────────────

void mp3_player_reset(void) {
    if (!s_initialized) return;
    mutex_enter_blocking(&s_mp3_mutex);
    stop_playback();
    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }
    memset(&s_player, 0, sizeof(s_player));
    s_player.volume = 100;
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_pcm_channels = 2;
    s_ring_rd = s_ring_wr = 0;
    if (s_mad_stream) mad_stream_init(s_mad_stream);
    if (s_mad_frame)  mad_frame_init(s_mad_frame);
    if (s_mad_synth)  mad_synth_init(s_mad_synth);
    mutex_exit(&s_mp3_mutex);
}

bool mp3_player_init(void) {
    if (s_initialized) return true;

    mutex_init(&s_mp3_mutex);
    printf("[MP3] Initializing libmad decoder...\n");

    if (!s_mad_stream) {
        s_mad_stream = umm_malloc(sizeof(struct mad_stream));
        if (!s_mad_stream) { printf("[MP3] FAILED to alloc mad_stream\n"); return false; }
    }
    if (!s_mad_frame) {
        s_mad_frame = umm_malloc(sizeof(struct mad_frame));
        if (!s_mad_frame) { printf("[MP3] FAILED to alloc mad_frame\n"); return false; }
    }
    if (!s_mad_synth) {
        s_mad_synth = umm_malloc(sizeof(struct mad_synth));
        if (!s_mad_synth) { printf("[MP3] FAILED to alloc mad_synth\n"); return false; }
    }

    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    mad_synth_init(s_mad_synth);

    printf("[MP3] libmad structs: stream=%u frame=%u synth=%u bytes\n",
           (unsigned)sizeof(struct mad_stream),
           (unsigned)sizeof(struct mad_frame),
           (unsigned)sizeof(struct mad_synth));

    if (!s_pcm_ring) {
        s_pcm_ring = umm_malloc(PCM_RING_SIZE);
        if (!s_pcm_ring) {
            printf("[MP3] FAILED to alloc ring buffer\n");
            return false;
        }
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

    mutex_enter_blocking(&s_mp3_mutex);

    stop_playback();

    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }

    s_file = sdcard_fopen(path, "rb");
    if (!s_file) {
        printf("mp3_player: failed to open %s\n", path);
        mutex_exit(&s_mp3_mutex);
        return false;
    }

    int rd = sdcard_fread(s_file, s_decode_buffer, MP3_DECODE_BUFFER_SIZE - MAD_BUFFER_GUARD);
    if (rd <= 0) {
        sdcard_fclose(s_file); s_file = NULL;
        mutex_exit(&s_mp3_mutex);
        return false;
    }
    s_bytes_in_buffer = rd;
    s_buffer_pos = 0;
    s_ring_rd = s_ring_wr = 0;

    // Zero-pad guard bytes
    memset(s_decode_buffer + s_bytes_in_buffer, 0, MAD_BUFFER_GUARD);

    // Re-init libmad state for clean decode
    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    mad_synth_init(s_mad_synth);

    // Probe first frame header
    mad_stream_buffer(s_mad_stream, s_decode_buffer, s_bytes_in_buffer + MAD_BUFFER_GUARD);
    if (mad_header_decode(&s_mad_frame->header, s_mad_stream) != 0) {
        printf("mp3_player: not an MP3 file (%s)\n", path);
        sdcard_fclose(s_file); s_file = NULL;
        mutex_exit(&s_mp3_mutex);
        return false;
    }

    player->sample_rate = s_mad_frame->header.samplerate;
    player->channels    = MAD_NCHANNELS(&s_mad_frame->header);
    player->length      = 0;
    player->position    = 0;
    s_pcm_channels      = player->channels;

    printf("mp3_player: loaded %s (%lu Hz, %lu ch)\n", path, (unsigned long)player->sample_rate, (unsigned long)player->channels);

    // Re-init for clean decode from beginning (header_decode consumed some bytes)
    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    mad_synth_init(s_mad_synth);
    // Reset buffer position to start
    s_buffer_pos = 0;

    mutex_exit(&s_mp3_mutex);
    return true;
}

bool mp3_player_play(mp3_player_t *player, uint8_t repeat_count) {
    (void)repeat_count;
    if (!player || !s_file) return false;

    mutex_enter_blocking(&s_mp3_mutex);

    player->playing = true;
    player->paused  = false;

    // Pre-fill ring buffer before starting playback
    s_ring_rd = s_ring_wr = 0;
    decode_fill_ring();

    // Configure PWM with fractional divider for accurate sample rate
    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
    s_pwm_slice = pwm_gpio_to_slice_num(AUDIO_PIN_L);

    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t target = player->sample_rate * (uint32_t)(PWM_WRAP + 1);
    uint32_t div_int = sys_clk / target;
    uint32_t remainder = sys_clk - div_int * target;
    uint32_t div_frac = (remainder * 16 + target / 2) / target;  // round to nearest
    if (div_int < 1) { div_int = 1; div_frac = 0; }

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, PWM_WRAP);
    pwm_config_set_clkdiv_int_frac(&cfg, div_int, div_frac);
    pwm_init(s_pwm_slice, &cfg, true);

    // Set up DMA channel paced by PWM wrap DREQ
    if (s_dma_chan < 0)
        s_dma_chan = dma_claim_unused_channel(true);

    dma_channel_config dc = dma_channel_get_default_config(s_dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, DREQ_PWM_WRAP0 + s_pwm_slice);

    dma_channel_configure(s_dma_chan, &dc,
        &pwm_hw->slice[s_pwm_slice].cc,   // write to PWM CC register
        s_dma_buf[0],                       // initial read buffer
        DMA_BUF_SAMPLES,                    // transfer count
        false);                             // don't start yet

    // Enable DMA IRQ (use DMA_IRQ_1 to avoid conflict with display DMA)
    dma_channel_set_irq1_enabled(s_dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_1, dma_audio_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    // Fade in from silence to avoid pop
    s_fade_state = FADE_IN;
    s_fade_pos = 0;
    s_stop_after_fade = false;

    // Pre-fill both DMA ping-pong buffers
    fill_dma_buffer(s_dma_buf[0], DMA_BUF_SAMPLES);
    fill_dma_buffer(s_dma_buf[1], DMA_BUF_SAMPLES);
    s_dma_active_buf = 0;
    s_dma_active = true;

    // Start DMA — audio begins playing
    dma_channel_start(s_dma_chan);
    mutex_exit(&s_mp3_mutex);
    return true;
}

void mp3_player_stop(mp3_player_t *player) {
    if (!player) return;

    mutex_enter_blocking(&s_mp3_mutex);

    // If DMA is active, fade out before stopping to avoid pop
    if (s_dma_active && player->playing) {
        s_fade_state = FADE_OUT;
        s_fade_pos = 0;
        s_stop_after_fade = true;

        // Wait for fade to complete (~1.5ms at 44.1kHz)
        mutex_exit(&s_mp3_mutex);
        sleep_ms(3);
        mutex_enter_blocking(&s_mp3_mutex);
    }

    player->playing  = false;
    player->paused   = false;
    player->position = 0;

    stop_playback();

    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }

    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_ring_rd = s_ring_wr = 0;

    mutex_exit(&s_mp3_mutex);
}

void mp3_player_pause(mp3_player_t *player) {
    if (!player) return;
    player->paused = true;
}

void mp3_player_resume(mp3_player_t *player) {
    if (!player) return;
    s_fade_state = FADE_IN;
    s_fade_pos = 0;
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

uint32_t mp3_player_get_sample_rate(const mp3_player_t *player) {
    if (!player) return 0;
    return player->sample_rate;
}

void mp3_player_set_loop(mp3_player_t *player, bool loop) {
    if (!player) return;
    player->loop = loop;
}

void mp3_player_update(void) {
    if (!s_initialized) return;
    if (!mutex_try_enter(&s_mp3_mutex, NULL)) return;
    if (s_player.playing && !s_player.paused) {
        decode_fill_ring();
    }
    mutex_exit(&s_mp3_mutex);
}
