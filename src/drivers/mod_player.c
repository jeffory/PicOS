// MOD music player — firmware-integrated, mirrors mp3_player.c pattern.
// Uses pocketmod (single-header MOD decoder) for decoding, and the
// audio_start_stream()/audio_push_samples() path for PCM output.
//
// Core 1 calls mod_player_update() every 5ms to render + push PCM.

#include "mod_player.h"
#include "audio.h"
#include "sdcard.h"
#include "pico/mutex.h"

#include "umm_malloc.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define POCKETMOD_IMPLEMENTATION
#include "pocketmod.h"

// Render at 22050 Hz — good enough for tracker music, halves CPU vs 44100
#define MOD_SAMPLE_RATE  22050

// Render buffer: 128 stereo frames per update tick (5ms @ 22050 = ~110 frames)
#define MOD_RENDER_FRAMES  128
#define MOD_RENDER_BUF_SIZE  (MOD_RENDER_FRAMES * 2 * sizeof(float))

struct mod_player {
    pocketmod_context ctx;
    uint8_t *mod_data;      // MOD file loaded into PSRAM (umm_malloc)
    uint32_t mod_size;
    bool playing;
    bool paused;
    bool loop;
    uint8_t volume;         // 0-100
};

// Heap-allocated to save ~4KB BSS (pocketmod_context is ~2.5KB alone)
static mod_player_t *s_player = NULL;
static bool s_initialized = false;
static mutex_t s_mod_mutex;

// Render buffers allocated in PSRAM alongside s_player
static float *s_render_buf = NULL;
static int16_t *s_pcm_buf = NULL;

bool mod_player_init(void) {
    if (s_initialized) return true;
    // Only init mutex here — PSRAM heap may not be ready yet.
    // Actual PSRAM allocations are deferred to mod_player_create().
    mutex_init(&s_mod_mutex);
    s_initialized = true;
    return true;
}

// Allocate player + render buffers in PSRAM (called lazily from create)
static bool mod_player_alloc(void) {
    if (s_player) return true;

    s_player = (mod_player_t *)umm_malloc(sizeof(mod_player_t));
    if (!s_player) {
        printf("[MOD] OOM allocating player\n");
        return false;
    }
    s_render_buf = (float *)umm_malloc(MOD_RENDER_FRAMES * 2 * sizeof(float));
    s_pcm_buf = (int16_t *)umm_malloc(MOD_RENDER_FRAMES * 2 * sizeof(int16_t));
    if (!s_render_buf || !s_pcm_buf) {
        umm_free(s_player); s_player = NULL;
        umm_free(s_render_buf); s_render_buf = NULL;
        umm_free(s_pcm_buf); s_pcm_buf = NULL;
        printf("[MOD] OOM allocating render buffers\n");
        return false;
    }

    memset(s_player, 0, sizeof(mod_player_t));
    s_player->volume = 100;
    return true;
}

void mod_player_deinit(void) {
    if (!s_initialized) return;
    mod_player_stop(s_player);
    if (s_player->mod_data) {
        umm_free(s_player->mod_data);
        s_player->mod_data = NULL;
    }
    umm_free(s_render_buf); s_render_buf = NULL;
    umm_free(s_pcm_buf); s_pcm_buf = NULL;
    umm_free(s_player); s_player = NULL;
    s_initialized = false;
}

void mod_player_reset(void) {
    if (!s_player) return;
    mod_player_stop(s_player);
    if (s_player->mod_data) {
        umm_free(s_player->mod_data);
        s_player->mod_data = NULL;
    }
    s_player->mod_size = 0;
}

mod_player_t *mod_player_create(void) {
    if (!s_initialized) mod_player_init();
    if (!mod_player_alloc()) return NULL;
    // Single instance — only one MOD can play at a time
    mod_player_reset();
    s_player->volume = 100;
    s_player->loop = false;
    return s_player;
}

void mod_player_destroy(mod_player_t *player) {
    if (!player) return;
    mod_player_stop(player);
    if (player->mod_data) {
        umm_free(player->mod_data);
        player->mod_data = NULL;
    }
    player->mod_size = 0;
}

bool mod_player_load(mod_player_t *player, const char *path) {
    if (!player || !path) return false;

    // Stop any current playback
    mod_player_stop(player);

    // Free previous MOD data
    if (player->mod_data) {
        umm_free(player->mod_data);
        player->mod_data = NULL;
        player->mod_size = 0;
    }

    // Open and read the MOD file from SD card via sdcard API
    // (works on both real hardware and simulator)
    int fsize = sdcard_fsize(path);
    if (fsize <= 0 || fsize > 512 * 1024) {
        printf("[MOD] File missing, empty, or too large: %s (%d bytes)\n", path, fsize);
        return false;
    }

    sdfile_t fil = sdcard_fopen(path, "rb");
    if (!fil) {
        printf("[MOD] Failed to open: %s\n", path);
        return false;
    }

    // Allocate in PSRAM
    player->mod_data = (uint8_t *)umm_malloc((uint32_t)fsize);
    if (!player->mod_data) {
        sdcard_fclose(fil);
        printf("[MOD] OOM allocating %d bytes\n", fsize);
        return false;
    }

    int bytes_read = sdcard_fread(fil, player->mod_data, fsize);
    sdcard_fclose(fil);

    if (bytes_read != fsize) {
        umm_free(player->mod_data);
        player->mod_data = NULL;
        printf("[MOD] Read error: read=%d expected=%d\n", bytes_read, fsize);
        return false;
    }

    player->mod_size = fsize;

    // Validate the MOD file by trying to init pocketmod
    if (!pocketmod_init(&player->ctx, player->mod_data, (int)fsize, MOD_SAMPLE_RATE)) {
        umm_free(player->mod_data);
        player->mod_data = NULL;
        player->mod_size = 0;
        printf("[MOD] Invalid MOD file: %s\n", path);
        return false;
    }

    printf("[MOD] Loaded: %s (%u bytes)\n", path, fsize);
    return true;
}

void mod_player_play(mod_player_t *player, bool loop) {
    if (!player || !player->mod_data) return;

    mutex_enter_blocking(&s_mod_mutex);

    player->loop = loop;
    player->paused = false;

    // Re-init pocketmod from the start
    pocketmod_init(&player->ctx, player->mod_data, (int)player->mod_size, MOD_SAMPLE_RATE);

    // Start audio output stream
    audio_start_stream(MOD_SAMPLE_RATE);

    player->playing = true;

    mutex_exit(&s_mod_mutex);
    printf("[MOD] Playing (loop=%d)\n", loop);
}

void mod_player_stop(mod_player_t *player) {
    if (!player) return;

    mutex_enter_blocking(&s_mod_mutex);

    if (player->playing) {
        player->playing = false;
        player->paused = false;
        audio_stop_stream();
    }

    mutex_exit(&s_mod_mutex);
}

void mod_player_pause(mod_player_t *player) {
    if (!player || !player->playing) return;
    player->paused = true;
}

void mod_player_resume(mod_player_t *player) {
    if (!player || !player->playing) return;
    player->paused = false;
}

bool mod_player_is_playing(const mod_player_t *player) {
    return player && player->playing && !player->paused;
}

void mod_player_set_volume(mod_player_t *player, uint8_t volume) {
    if (!player) return;
    if (volume > 100) volume = 100;
    player->volume = volume;
}

uint8_t mod_player_get_volume(const mod_player_t *player) {
    return player ? player->volume : 0;
}

void mod_player_set_loop(mod_player_t *player, bool loop) {
    if (!player) return;
    player->loop = loop;
}

// Called from Core 1 every ~1ms
void mod_player_update(void) {
    if (!s_initialized) return;

    mod_player_t *p = s_player;
    if (!p->playing || p->paused || !p->mod_data) return;

    // Flow control: only render when ring buffer has room.
    // Without this, pocketmod advances its clock but samples are dropped
    // by audio_push_samples() when the ring is full — causing severe corruption.
    uint32_t free = audio_ring_free();
    if (free < MOD_RENDER_FRAMES)
        return;

    // Render a chunk of float stereo PCM via pocketmod
    int bytes_rendered = pocketmod_render(&p->ctx, s_render_buf,
                                           (int)MOD_RENDER_BUF_SIZE);
    int frames = bytes_rendered / (int)(2 * sizeof(float));

    if (frames <= 0) {
        // Song finished
        if (p->loop) {
            // Re-init to restart from beginning
            pocketmod_init(&p->ctx, p->mod_data, (int)p->mod_size, MOD_SAMPLE_RATE);
            return;  // will render next tick
        }
        p->playing = false;
        audio_stop_stream();
        return;
    }

    // Check if song has looped (pocketmod tracks this internally)
    if (!p->loop && pocketmod_loop_count(&p->ctx) > 0) {
        p->playing = false;
        audio_stop_stream();
        return;
    }

    // Convert float [-1,1] stereo to int16_t stereo with volume scaling.
    // pocketmod normalizes output across channels (4-ch MOD = ±0.25 peak),
    // so apply 2x gain to bring levels into audible range without clipping.
    uint32_t vol = (uint32_t)p->volume * 256 / 100;  // 0..256
    for (int i = 0; i < frames * 2; i++) {
        float sample = s_render_buf[i];
        int32_t pcm = (int32_t)(sample * (32767.0f * 3.0f));
        pcm = (pcm * (int32_t)vol) >> 8;
        if (pcm > 32767) pcm = 32767;
        if (pcm < -32768) pcm = -32768;
        s_pcm_buf[i] = (int16_t)pcm;
    }

    // Push to audio output ring buffer
    audio_push_samples(s_pcm_buf, frames);

    // Periodic debug: log DMA stats every ~2 seconds
    static uint32_t s_debug_counter = 0;
    if (++s_debug_counter >= 400) {
        s_debug_counter = 0;
        uint32_t isr_cnt, underruns, ring_used;
        audio_stream_debug(&isr_cnt, &underruns, &ring_used);
        uint32_t ring_free = audio_ring_free();
        printf("[MOD] DMA ISR=%lu underruns=%lu ring=%lu free=%lu\n",
               isr_cnt, underruns, ring_used, ring_free);
    }
}
