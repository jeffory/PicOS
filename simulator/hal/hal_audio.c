// HAL Audio - SDL2 Audio Implementation (Stub)

#include "hal_audio.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

// Audio configuration
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_CHANNELS 2
#define AUDIO_BUFFER_SIZE 4096

static int g_initialized = 0;

bool hal_audio_init(void) {
    SDL_AudioSpec want, have;
    
    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = AUDIO_CHANNELS;
    want.samples = 512;
    want.callback = NULL;  // We'll use SDL_QueueAudio
    
    if (SDL_OpenAudio(&want, &have) < 0) {
        fprintf(stderr, "Failed to open audio: %s\n", SDL_GetError());
        return false;
    }
    
    SDL_PauseAudio(0);  // Start audio
    g_initialized = 1;
    printf("[Audio] Initialized (%d Hz, %d channels)\n", have.freq, have.channels);
    return true;
}

void hal_audio_shutdown(void) {
    if (g_initialized) {
        SDL_CloseAudio();
        g_initialized = 0;
        printf("[Audio] Shutdown\n");
    }
}

// hal_audio_update() is implemented in sim_audio.c — it drives tone
// generation, sound sample mixing, fileplayer streaming, and MP3 decode.

int hal_audio_push_samples(const int16_t* samples, int count) {
    if (!g_initialized || !samples || count <= 0) return 0;
    int bytes = count * sizeof(int16_t) * AUDIO_CHANNELS;
    return SDL_QueueAudio(1, samples, bytes) == 0 ? count : 0;
}

int hal_audio_buffer_space(void) {
    if (!g_initialized) return 0;
    
    // Return approximate queue space
    return AUDIO_BUFFER_SIZE - SDL_GetQueuedAudioSize(1) / sizeof(int16_t);
}
