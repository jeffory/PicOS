#include "mp3_player.h"
#include "audio.h"
#include "../hardware.h"
#include "sdcard.h"
#include "ff.h"       // direct FatFS calls for non-blocking SD reads
#include "pico/platform.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "pico/mutex.h"
#include "pico/time.h"
#include "pio_psram.h"

#define FPM_DEFAULT
#include "mad.h"

#include "umm_malloc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MP3_DECODE_BUFFER_SIZE (8192 + MAD_BUFFER_GUARD)
#define PCM_RING_SIZE          32768
#define DMA_BUF_SAMPLES        256

// PWM_WRAP chosen so that at 300 MHz (video playback clock), common sample
// rates land on clean integer dividers:  44100 Hz → div=4, 22050 Hz → div=8.
// Period 1700 gives 0.04% timing error vs 0.28% with period 2048.
#define PWM_WRAP  1699
#define PWM_MID   ((PWM_WRAP + 1) / 2)  // 850

#define FADE_SAMPLES 64

typedef enum {
    FADE_NONE,
    FADE_IN,
    FADE_OUT,
} fade_state_t;

static volatile fade_state_t s_fade_state = FADE_NONE;
static volatile int          s_fade_pos   = 0;
static volatile bool         s_stop_after_fade = false;

static struct mad_stream *s_mad_stream = NULL;
static struct mad_frame  *s_mad_frame  = NULL;
static struct mad_synth  *s_mad_synth  = NULL;
static sdfile_t    s_file = NULL;
static uint8_t     s_decode_buffer[MP3_DECODE_BUFFER_SIZE] __attribute__((aligned(4)));
static int         s_bytes_in_buffer = 0;
static int         s_buffer_pos = 0;

static mp3_player_t s_player;
static bool         s_initialized = false;
static mutex_t      s_mp3_mutex;
#define MAX_ERRORS_PER_UPDATE  32

static uint8_t *s_pcm_ring = NULL;
static _Atomic size_t s_ring_rd = 0;
static _Atomic size_t s_ring_wr = 0;
static bool s_use_pio_psram = false;
static uint32_t s_pio_psram_base = 0;

#define STAGING_BUF_SIZE  (DMA_BUF_SAMPLES * 4 * 4)
static uint8_t  s_staging_buf[STAGING_BUF_SIZE] __attribute__((aligned(4)));
static size_t   s_staging_avail = 0;
static size_t   s_staging_pos   = 0;

static uint8_t s_pio_read_buf[STAGING_BUF_SIZE] __attribute__((aligned(4)));

// ── Fed mode: compressed MP3 ring in QMI PSRAM, written by Core 0 (video) ───
// Uses umm_malloc (not PIO PSRAM) because both cores access this ring
// concurrently and PIO1 SPI is not thread-safe across cores.
#define FED_RING_SIZE  (64 * 1024)
static bool     s_fed_mode = false;
static uint8_t *s_fed_ring_buf = NULL; // umm_malloc'd in QMI PSRAM
static _Atomic uint32_t s_fed_wr = 0; // written by Core 0
static _Atomic uint32_t s_fed_rd = 0; // read by Core 1

static inline uint32_t fed_ring_available(void) {
    uint32_t wr = atomic_load_explicit(&s_fed_wr, memory_order_acquire);
    uint32_t rd = atomic_load_explicit(&s_fed_rd, memory_order_acquire);
    return (wr >= rd) ? (wr - rd) : (FED_RING_SIZE - rd + wr);
}

static inline uint32_t fed_ring_free(void) {
    return FED_RING_SIZE - 1 - fed_ring_available();
}

static void fed_ring_read(uint8_t *dst, uint32_t len) {
    uint32_t rd = atomic_load_explicit(&s_fed_rd, memory_order_acquire);
    uint32_t to_end = FED_RING_SIZE - rd;
    if (len <= to_end) {
        memcpy(dst, s_fed_ring_buf + rd, len);
    } else {
        memcpy(dst, s_fed_ring_buf + rd, to_end);
        memcpy(dst + to_end, s_fed_ring_buf, len - to_end);
    }
    atomic_store_explicit(&s_fed_rd, (rd + len) % FED_RING_SIZE, memory_order_release);
}

static inline size_t __time_critical_func(ring_available)(void) {
    size_t wr = atomic_load_explicit(&s_ring_wr, memory_order_acquire);
    size_t rd = atomic_load_explicit(&s_ring_rd, memory_order_acquire);
    return (wr >= rd) ? (wr - rd) : (PCM_RING_SIZE - rd + wr);
}

static inline size_t __time_critical_func(ring_free)(void) {
    return PCM_RING_SIZE - 1 - ring_available();
}

static void ring_write(const uint8_t *data, size_t len) {
    while (len > 0) {
        size_t wr = atomic_load_explicit(&s_ring_wr, memory_order_relaxed);
        size_t free_space = ring_free();
        if (free_space == 0) break;

        size_t chunk = (len < free_space) ? len : free_space;
        size_t to_end = PCM_RING_SIZE - wr;

        if (s_use_pio_psram) {
            if (chunk <= to_end) {
                pio_psram_write(s_pio_psram_base + wr, data, chunk);
            } else {
                pio_psram_write(s_pio_psram_base + wr, data, to_end);
                pio_psram_write(s_pio_psram_base, data + to_end, chunk - to_end);
            }
        } else {
            if (chunk <= to_end) {
                memcpy(s_pcm_ring + wr, data, chunk);
            } else {
                memcpy(s_pcm_ring + wr, data, to_end);
                memcpy(s_pcm_ring, data + to_end, chunk - to_end);
            }
        }
        atomic_store_explicit(&s_ring_wr, (wr + chunk) % PCM_RING_SIZE, memory_order_release);
        data += chunk;
        len -= chunk;
    }
}

// ── DMA audio output (paced by PWM DREQ — zero jitter) ─────────────────────
static int  s_dma_chan = -1;
static uint32_t s_dma_buf[2][DMA_BUF_SAMPLES];  // ping-pong SRAM buffers
static volatile int s_dma_active_buf = 0;
static volatile bool s_dma_active = false;
static int  s_pcm_channels = 2;
static unsigned int s_pwm_slice = 0;

// Deferred DMA start: set on Core 0, consumed on Core 1 so the DMA ISR
// fires on Core 1 instead of preempting the game loop on Core 0.
static volatile bool s_dma_start_pending = false;
static bool          s_irq_on_core1 = false;

// ── Refill the SRAM staging buffer from the PCM ring ────────────────────────
// Called from mp3_player_update() (Core 1, non-ISR context) to keep the
// staging buffer topped up before the decode cycle begins.
static void refill_staging_buf(void) {
    if (s_staging_avail >= STAGING_BUF_SIZE / 2)
        return;

    if (s_staging_pos > 0 && s_staging_avail > 0) {
        memmove(s_staging_buf, s_staging_buf + s_staging_pos, s_staging_avail);
    }
    s_staging_pos = 0;

    size_t space = STAGING_BUF_SIZE - s_staging_avail;
    size_t avail = ring_available();
    size_t to_read = (space < avail) ? space : avail;
    if (to_read == 0) return;

    size_t rd = atomic_load_explicit(&s_ring_rd, memory_order_relaxed);
    size_t to_end = PCM_RING_SIZE - rd;

    if (s_use_pio_psram) {
        if (to_read <= to_end) {
            pio_psram_read(s_pio_psram_base + rd, s_staging_buf + s_staging_avail, to_read);
        } else {
            pio_psram_read(s_pio_psram_base + rd, s_staging_buf + s_staging_avail, to_end);
            pio_psram_read(s_pio_psram_base, s_staging_buf + s_staging_avail + to_end, to_read - to_end);
        }
    } else {
        if (to_read <= to_end) {
            memcpy(s_staging_buf + s_staging_avail, s_pcm_ring + rd, to_read);
        } else {
            memcpy(s_staging_buf + s_staging_avail, s_pcm_ring + rd, to_end);
            memcpy(s_staging_buf + s_staging_avail + to_end, s_pcm_ring, to_read - to_end);
        }
    }
    atomic_store_explicit(&s_ring_rd, (rd + to_read) % PCM_RING_SIZE, memory_order_release);
    s_staging_avail += to_read;
}

// ── Fill one DMA buffer from the staging buffer (called from DMA ISR) ───────
// Reads only from SRAM (s_staging_buf) — never touches PIO PSRAM directly.
// Precomputed volume scale: vol_scale = volume * 256 / 100, so
// (sample * vol_scale) >> 8  ≈  sample * volume / 100
// Updated whenever mp3_player_set_volume() is called.
static uint32_t s_vol_scale = 256;  // 256 = 100%

static void __time_critical_func(fill_dma_buffer)(uint32_t *buf, int count) {
    size_t bytes_per_pair = (s_pcm_channels > 1) ? 4 : 2;
    uint32_t vol_scale = s_vol_scale;

    for (int i = 0; i < count; i++) {
        int32_t lv, rv;

        if (s_staging_avail < bytes_per_pair) {
            lv = PWM_MID;
            rv = PWM_MID;
        } else {
            uint8_t raw[4];
            memcpy(raw, s_staging_buf + s_staging_pos, bytes_per_pair);
            s_staging_pos   += bytes_per_pair;
            s_staging_avail -= bytes_per_pair;

            int16_t left, right;
            memcpy(&left, raw, 2);
            if (s_pcm_channels > 1)
                memcpy(&right, raw + 2, 2);
            else
                right = left;

            // 16-bit signed → PWM range (0–PWM_WRAP), then apply volume
            lv = (int32_t)(((uint32_t)(left  + 32768) * (PWM_WRAP + 1)) >> 16);
            rv = (int32_t)(((uint32_t)(right + 32768) * (PWM_WRAP + 1)) >> 16);
            lv = (lv * vol_scale) >> 8;
            rv = (rv * vol_scale) >> 8;
            if (lv > PWM_WRAP) lv = PWM_WRAP;
            if (rv > PWM_WRAP) rv = PWM_WRAP;
        }

        // Apply fade envelope
        // FADE_SAMPLES=64, so *256/64 == *4 == <<2 (no division needed)
        if (s_fade_state == FADE_IN) {
            int32_t gain = s_fade_pos << 2;
            lv = PWM_MID + (((lv - PWM_MID) * gain) >> 8);
            rv = PWM_MID + (((rv - PWM_MID) * gain) >> 8);
            if (++s_fade_pos >= FADE_SAMPLES)
                s_fade_state = FADE_NONE;
        } else if (s_fade_state == FADE_OUT) {
            int32_t gain = (FADE_SAMPLES - s_fade_pos) << 2;
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
// With Fix 2 this ISR fires on Core 1 (registered via deferred start),
// keeping the game loop on Core 0 free from audio interrupt overhead.
static void dma_audio_irq_handler(void) {
    dma_hw->ints1 = 1u << s_dma_chan;          // clear IRQ (using DMA_IRQ_1)

    if (!s_dma_active || !s_player.playing) {
        // Stopped or paused: silence outputs, don't restart DMA
        pwm_set_gpio_level(AUDIO_PIN_L, PWM_MID);
        pwm_set_gpio_level(AUDIO_PIN_R, PWM_MID);
        s_dma_active = false;
        return;
    }

    // Swap to the pre-filled buffer and start DMA immediately
    int next_buf = s_dma_active_buf ^ 1;
    dma_channel_set_read_addr(s_dma_chan, s_dma_buf[next_buf], true);

    // Refill the buffer that just finished playing
    fill_dma_buffer(s_dma_buf[s_dma_active_buf], DMA_BUF_SAMPLES);
    s_player.position += DMA_BUF_SAMPLES;
    s_dma_active_buf = next_buf;
}

// ── Refill compressed-data buffer from SD card or fed ring ────────────────────
static bool refill_decode_buffer(void) {
    // Shift leftover data to front
    if (s_buffer_pos > 0 && s_bytes_in_buffer > 0) {
        memmove(s_decode_buffer, s_decode_buffer + s_buffer_pos, s_bytes_in_buffer);
    }
    s_buffer_pos = 0;

    int space = (int)MP3_DECODE_BUFFER_SIZE - s_bytes_in_buffer - MAD_BUFFER_GUARD;
    if (space > 0) {
        if (s_fed_mode) {
            // Fed mode: read from compressed audio ring in PIO PSRAM
            uint32_t avail = fed_ring_available();
            uint32_t to_read = ((uint32_t)space < avail) ? (uint32_t)space : avail;
            if (to_read > 4096) to_read = 4096;
            if (to_read > 0) {
                fed_ring_read(s_decode_buffer + s_bytes_in_buffer, to_read);
                s_bytes_in_buffer += (int)to_read;
            }
        } else {
            // SD mode: non-blocking read
            if (!s_file) goto pad;
            if (!recursive_mutex_try_enter(&g_sdcard_mutex, NULL))
                goto pad;
            int to_read = (space > 4096) ? 4096 : space;
            UINT br = 0;
            FRESULT res = f_read((FIL *)s_file, s_decode_buffer + s_bytes_in_buffer, to_read, &br);
            recursive_mutex_exit(&g_sdcard_mutex);
            if (res == FR_OK && br > 0)
                s_bytes_in_buffer += (int)br;
        }
    }

pad:
    // Zero-pad guard bytes for libmad
    memset(s_decode_buffer + s_bytes_in_buffer, 0, MAD_BUFFER_GUARD);

    return s_bytes_in_buffer > 0;
}

// ── Decode: fill PCM ring buffer (called from main loop, NOT ISR) ───────────
static void decode_fill_ring(void) {
    if (!s_player.playing || s_player.paused || !s_mad_stream)
        return;
    if (!s_fed_mode && !s_file)
        return;

    // Batch decode: only decode when ring buffer is below 50% capacity,
    // then decode up to 3 frames to refill quickly.  This creates bursty
    // PSRAM access (~10ms decode burst, ~30-40ms idle) instead of constant
    // pressure every 5ms, reducing QMI contention with Core 0's XIP cache.
    size_t avail = ring_available();
    if (avail > PCM_RING_SIZE / 2)
        return;  // ring buffer is >50% full, skip this cycle

    int max_frames = 3;
    int frames_decoded = 0;
    int errors_this_update = 0;
    while (frames_decoded < max_frames && ring_free() >= 1152 * 2 * 2) {
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
                    if (s_fed_mode) {
                        // Fed mode: no more data right now, just break
                        break;
                    }
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
                // For LOSTSYNC with low buffer, try to refill first
                if (s_mad_stream->error == MAD_ERROR_LOSTSYNC && s_bytes_in_buffer < 256) {
                    if (!refill_decode_buffer()) {
                        if (s_fed_mode) {
                            break;
                        }
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
                
                // Allow more recoverable errors - don't count toward the error limit
                // just continue to next frame
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
        frames_decoded++;

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
    s_dma_start_pending = false;           // cancel any deferred start
    if (s_dma_chan >= 0) {
        dma_channel_set_irq1_enabled(s_dma_chan, false); // prevent ISR restart
        dma_channel_abort(s_dma_chan);
    }
    s_dma_active = false;
    // Midpoint is true silence for AC-coupled output (no pop)
    pwm_set_gpio_level(AUDIO_PIN_L, PWM_MID);
    pwm_set_gpio_level(AUDIO_PIN_R, PWM_MID);
    s_fade_state = FADE_NONE;
    s_stop_after_fade = false;
}

// ── Helper to skip ID3v2 tags ───────────────────────────────────────────────
static int skip_id3v2tag(struct mad_stream *stream) {
    const unsigned char *ptr = stream->buffer;
    size_t len = stream->bufend - stream->buffer;

    if (len < 10) return 0;

    // ID3v2 header: "ID3" (3 bytes), version (2 bytes), flags (1 byte), size (4 bytes)
    if (ptr[0] == 'I' && ptr[1] == 'D' && ptr[2] == '3') {
        // Size is 4 syncsafe bytes (msb is always 0)
        unsigned long size = 
            ((unsigned long)(ptr[6] & 0x7f) << 21) |
            ((unsigned long)(ptr[7] & 0x7f) << 14) |
            ((unsigned long)(ptr[8] & 0x7f) << 7) |
            ((unsigned long)(ptr[9] & 0x7f));

        size += 10; // header size

        // If footer flag is set (bit 4 of flags byte 5), there's a 10-byte footer
        if (ptr[5] & 0x10) size += 10;

        if (size > len) size = len; // sanity check

        mad_stream_skip(stream, size);
        return (int)size;
    }

    return 0;
}

// ── Public API ──────────────────────────────────────────────────────────────

void mp3_player_reset(void) {
    if (!s_initialized) return;
    if (s_fed_mode) mp3_player_stop_fed();
    mutex_enter_blocking(&s_mp3_mutex);
    stop_playback();
    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }
    memset(&s_player, 0, sizeof(s_player));
    s_player.volume = 100;
    s_vol_scale = 256;
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_pcm_channels = 2;

    s_ring_rd = s_ring_wr = 0;
    s_staging_avail = 0;
    s_staging_pos = 0;
    if (s_mad_stream) mad_stream_init(s_mad_stream);
    if (s_mad_frame)  mad_frame_init(s_mad_frame);
    if (s_mad_synth)  mad_synth_init(s_mad_synth);
    mutex_exit(&s_mp3_mutex);
}

void mp3_player_deinit(void) {
    if (!s_initialized) return;
    mutex_enter_blocking(&s_mp3_mutex);
    stop_playback();
    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }
    if (s_mad_stream) { umm_free(s_mad_stream); s_mad_stream = NULL; }
    if (s_mad_frame)  { umm_free(s_mad_frame); s_mad_frame = NULL; }
    if (s_mad_synth)  { umm_free(s_mad_synth); s_mad_synth = NULL; }
    if (s_pcm_ring)   { umm_free(s_pcm_ring); s_pcm_ring = NULL; }
    s_use_pio_psram = false;
    s_initialized = false;
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

    if (!s_pcm_ring && !s_use_pio_psram) {
        if (pio_psram_available()) {
            s_use_pio_psram = true;
            s_pio_psram_base = PIO_PSRAM_MP3_RING_BASE;
            printf("[MP3] PCM ring buffer → PIO PSRAM @ 0x%lx (%d bytes, 8KB bulk xfers)\n",
                   (unsigned long)s_pio_psram_base, PCM_RING_SIZE);
        } else {
            s_pcm_ring = umm_malloc(PCM_RING_SIZE);
            if (!s_pcm_ring) {
                printf("[MP3] FAILED to alloc ring buffer\n");
                return false;
            }
            s_use_pio_psram = false;
            printf("[MP3] PCM ring buffer → QMI PSRAM (umm_malloc, %d bytes)\n", PCM_RING_SIZE);
        }
    }

    memset(&s_player, 0, sizeof(s_player));
    s_player.volume = 100;
    s_vol_scale = 256;
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

    // Stop fed mode if active (video audio and file playback are mutually exclusive)
    if (s_fed_mode) mp3_player_stop_fed();

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

    // Probe first frame header (skipping ID3 tags if present)
    mad_stream_buffer(s_mad_stream, s_decode_buffer, s_bytes_in_buffer + MAD_BUFFER_GUARD);
    
    // Skip ID3v2 tags (mad_header_decode doesn't do this automatically)
    int tag_len = skip_id3v2tag(s_mad_stream);
    if (tag_len > 0) {
        // consumed from s_decode_buffer
        s_buffer_pos += tag_len;
        s_bytes_in_buffer -= tag_len;
    }

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

// ── Setup PWM + DMA and request deferred start on Core 1 ─────────────────────
// Configures all hardware but does NOT start DMA or register the IRQ.
// Sets s_dma_start_pending so mp3_player_update() on Core 1 finishes the job,
// ensuring the DMA ISR fires on Core 1 (not the game loop core).
static void setup_playback_hw(void) {
    // Stop any audio.c DMA streaming before we reconfigure the PWM slice
    audio_stop_stream();

    // Configure PWM with fractional divider for accurate sample rate
    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
    s_pwm_slice = pwm_gpio_to_slice_num(AUDIO_PIN_L);

    uint32_t sys_clk = clock_get_hz(clk_sys);
    uint32_t target = s_player.sample_rate * (uint32_t)(PWM_WRAP + 1);
    uint32_t div_int = sys_clk / target;
    uint32_t remainder = sys_clk - div_int * target;
    uint32_t div_frac = (remainder * 16 + target / 2) / target;
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
        &pwm_hw->slice[s_pwm_slice].cc,
        s_dma_buf[0],
        DMA_BUF_SAMPLES,
        false);                             // don't start yet

    // Enable DMA IRQ for this channel (shared DMA register, core-independent)
    dma_channel_set_irq1_enabled(s_dma_chan, true);

    // Fade in from silence to avoid pop
    s_fade_state = FADE_IN;
    s_fade_pos = 0;
    s_stop_after_fade = false;

    // Pre-fill both DMA ping-pong buffers
    fill_dma_buffer(s_dma_buf[0], DMA_BUF_SAMPLES);
    fill_dma_buffer(s_dma_buf[1], DMA_BUF_SAMPLES);
    s_dma_active_buf = 0;

    // Signal Core 1 to register IRQ handler and start DMA
    s_dma_start_pending = true;
}

bool mp3_player_play(mp3_player_t *player, uint8_t repeat_count) {
    (void)repeat_count;
    if (!player || !s_file) return false;

    mutex_enter_blocking(&s_mp3_mutex);

    player->playing = true;
    player->paused  = false;

    // Pre-fill ring buffer before starting playback
    s_ring_rd = s_ring_wr = 0;
    s_staging_avail = 0;
    s_staging_pos = 0;
    decode_fill_ring();
    refill_staging_buf();

    setup_playback_hw();

    mutex_exit(&s_mp3_mutex);
    return true;
}

void mp3_player_stop(mp3_player_t *player) {
    if (!player) return;

    // If in fed mode, delegate to stop_fed
    if (s_fed_mode) {
        mp3_player_stop_fed();
        return;
    }

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
    s_staging_avail = 0;
    s_staging_pos = 0;

    mutex_exit(&s_mp3_mutex);
}

void mp3_player_pause(mp3_player_t *player) {
    if (!player || !player->playing) return;
    mutex_enter_blocking(&s_mp3_mutex);
    player->paused = true;
    stop_playback();       // abort DMA + silence PWM — zero ISR overhead
    mutex_exit(&s_mp3_mutex);
}

void mp3_player_resume(mp3_player_t *player) {
    if (!player || !player->paused || !player->playing) return;
    mutex_enter_blocking(&s_mp3_mutex);
    player->paused = false;
    setup_playback_hw();   // reconfigure PWM + DMA, deferred start on Core 1
    mutex_exit(&s_mp3_mutex);
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
    s_vol_scale = (uint32_t)volume * 256 / 100;
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

// ── Fed mode API (video player audio) ─────────────────────────────────────────

bool mp3_player_start_fed(uint32_t sample_rate, uint16_t channels) {
    if (!s_initialized) {
        if (!mp3_player_init()) return false;
    }

    mutex_enter_blocking(&s_mp3_mutex);

    // Stop any current playback
    stop_playback();
    if (s_file) { sdcard_fclose(s_file); s_file = NULL; }

    // Init fed ring in QMI PSRAM
    if (!s_fed_ring_buf) {
        s_fed_ring_buf = (uint8_t *)umm_malloc(FED_RING_SIZE);
        if (!s_fed_ring_buf) {
            printf("[MP3] FAILED to alloc fed ring (%d bytes)\n", FED_RING_SIZE);
            mutex_exit(&s_mp3_mutex);
            return false;
        }
    }
    s_fed_mode = true;
    s_fed_wr = 0;
    s_fed_rd = 0;

    // Reset decode state
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_ring_rd = s_ring_wr = 0;
    s_staging_avail = 0;
    s_staging_pos = 0;

    // Init libmad
    mad_stream_init(s_mad_stream);
    mad_frame_init(s_mad_frame);
    mad_synth_init(s_mad_synth);

    // Configure player state
    memset(&s_player, 0, sizeof(s_player));
    s_player.sample_rate = sample_rate;
    s_player.channels = channels;
    s_player.playing = true;
    s_player.volume = 100;
    s_vol_scale = 256;
    s_pcm_channels = (int)channels;

    printf("[MP3] Fed mode started: %u Hz, %u ch\n",
           (unsigned)sample_rate, (unsigned)channels);

    mutex_exit(&s_mp3_mutex);
    return true;
}

void mp3_player_start_dma_fed(void) {
    if (!s_fed_mode || !s_initialized) return;

    mutex_enter_blocking(&s_mp3_mutex);

    // Decode compressed data from fed ring into PCM ring
    decode_fill_ring();
    // Copy PCM data to SRAM staging buffer
    refill_staging_buf();
    // Configure PWM/DMA with real audio in the buffers (deferred start on Core 1)
    setup_playback_hw();

    mutex_exit(&s_mp3_mutex);
}

uint32_t mp3_player_feed(const uint8_t *data, uint32_t len) {
    if (!s_fed_ring_buf) return 0;
    uint32_t free_space = fed_ring_free();
    uint32_t to_write = (len < free_space) ? len : free_space;
    if (to_write == 0) return 0;

    uint32_t wr = atomic_load_explicit(&s_fed_wr, memory_order_relaxed);
    uint32_t to_end = FED_RING_SIZE - wr;
    if (to_write <= to_end) {
        memcpy(s_fed_ring_buf + wr, data, to_write);
    } else {
        memcpy(s_fed_ring_buf + wr, data, to_end);
        memcpy(s_fed_ring_buf, data + to_end, to_write - to_end);
    }
    atomic_store_explicit(&s_fed_wr, (wr + to_write) % FED_RING_SIZE, memory_order_release);
    return to_write;
}

void mp3_player_stop_fed(void) {
    if (!s_fed_mode) return;

    mutex_enter_blocking(&s_mp3_mutex);

    // Fade out if playing
    if (s_dma_active && s_player.playing) {
        s_fade_state = FADE_OUT;
        s_fade_pos = 0;
        s_stop_after_fade = true;
        mutex_exit(&s_mp3_mutex);
        sleep_ms(3);
        mutex_enter_blocking(&s_mp3_mutex);
    }

    stop_playback();
    s_player.playing = false;
    s_fed_mode = false;
    s_fed_wr = 0;
    s_fed_rd = 0;
    if (s_fed_ring_buf) { umm_free(s_fed_ring_buf); s_fed_ring_buf = NULL; }
    s_bytes_in_buffer = 0;
    s_buffer_pos = 0;
    s_ring_rd = s_ring_wr = 0;
    s_staging_avail = 0;
    s_staging_pos = 0;

    printf("[MP3] Fed mode stopped\n");
    mutex_exit(&s_mp3_mutex);
}

uint32_t mp3_player_feed_space(void) {
    return fed_ring_free();
}

bool mp3_player_is_fed_mode(void) {
    return s_fed_mode;
}

void mp3_player_update(void) {
    if (!s_initialized) return;
    if (!mutex_try_enter(&s_mp3_mutex, NULL)) return;

    // Deferred DMA start: register the IRQ handler on THIS core (Core 1)
    // so the audio ISR never preempts the game loop on Core 0.
    if (s_dma_start_pending) {
        if (!s_irq_on_core1) {
            irq_set_exclusive_handler(DMA_IRQ_1, dma_audio_irq_handler);
            irq_set_enabled(DMA_IRQ_1, true);
            s_irq_on_core1 = true;
        }
        s_dma_active = true;
        dma_channel_start(s_dma_chan);
        s_dma_start_pending = false;
    }

    if (s_player.playing && !s_player.paused) {
        refill_staging_buf();
        decode_fill_ring();
    }
    mutex_exit(&s_mp3_mutex);
}
