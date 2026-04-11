/*
  DOS86 — Audio mixer.

  Runs on Core 1 every 5ms via PicOS setAudioCallback(). Generates:
    1. PC speaker square wave (from speaker.c)
    2. OPL2 FM synthesis (from opl.c)
  Mixes both mono sources into stereo interleaved int16_t and pushes
  to PicOS via api->audio->pushSamples().

  Sample rate: 22050 Hz. At 5ms callback interval, we generate ~110 samples
  per callback. We use a fixed 128-sample buffer for simplicity.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#include "audio_mix.h"
#include "opl.h"
#include "speaker.h"
#include "os.h"

#define AUDIO_SAMPLE_RATE  22050u
#define MIX_BUF_SAMPLES   128       /* Samples per callback (~5.8ms at 22050) */

/* Saved API pointer (set at init, read on Core 1) */
static const PicoCalcAPI *s_api;

/* Work buffers — speaker_generate writes stereo interleaved,
   opl_generate writes mono. We mix into a stereo output buffer. */
static int16_t s_speaker_buf[MIX_BUF_SAMPLES * 2]; /* Stereo interleaved */
static int16_t s_opl_buf[MIX_BUF_SAMPLES];          /* Mono */
static int16_t s_mix_buf[MIX_BUF_SAMPLES * 2];      /* Stereo output */

/* Audio callback — runs on Core 1 every 5ms.
   Must not touch display, filesystem, or keyboard (Core 0 only). */
static void audio_callback(void)
{
    if (!s_api) return;

    /* Generate PC speaker output (stereo interleaved) */
    speaker_generate(s_speaker_buf, MIX_BUF_SAMPLES, AUDIO_SAMPLE_RATE);

    /* Generate OPL2 output (mono) */
    opl_generate(s_opl_buf, MIX_BUF_SAMPLES);

    /* Mix: speaker (stereo) + OPL2 (mono→stereo) → stereo output */
    for (int i = 0; i < MIX_BUF_SAMPLES; i++) {
        int32_t opl_sample = (int32_t)s_opl_buf[i];

        /* Left channel: speaker L + OPL2 */
        int32_t left  = (int32_t)s_speaker_buf[i * 2 + 0] + opl_sample;
        /* Right channel: speaker R + OPL2 */
        int32_t right = (int32_t)s_speaker_buf[i * 2 + 1] + opl_sample;

        /* Clamp to int16_t */
        if (left  >  32767) left  =  32767;
        if (left  < -32768) left  = -32768;
        if (right >  32767) right =  32767;
        if (right < -32768) right = -32768;

        s_mix_buf[i * 2 + 0] = (int16_t)left;
        s_mix_buf[i * 2 + 1] = (int16_t)right;
    }

    /* Push to PicOS audio (stereo frames, count = number of stereo frames) */
    s_api->audio->pushSamples(s_mix_buf, MIX_BUF_SAMPLES);
}

void audio_mix_init(const struct PicoCalcAPI *api)
{
    s_api = api;

    /* Initialize OPL2 at our output sample rate */
    opl_init(AUDIO_SAMPLE_RATE);

    /* Start PCM streaming at 22050 Hz */
    api->audio->startStream(AUDIO_SAMPLE_RATE);

    /* Register the audio callback to run on Core 1 every 5ms */
    api->sys->setAudioCallback(audio_callback);

    api->sys->log("DOS86: Audio mixer started (22050 Hz, speaker+OPL2)");
}

void audio_mix_shutdown(void)
{
    if (!s_api) return;

    /* Deregister callback first */
    s_api->sys->setAudioCallback((void (*)(void))0);

    /* Stop PCM stream */
    s_api->audio->stopStream();

    /* Shut down OPL2 */
    opl_shutdown();

    s_api->sys->log("DOS86: Audio mixer stopped");
    s_api = (void *)0;
}
