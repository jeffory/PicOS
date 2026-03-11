// PicOS sound module for DOOM
// Software mixer: decodes DMX lumps, mixes 8 channels, outputs stereo PCM
// via pushSamples() at 11025 Hz.
//
// Audio production is fully autonomous on Core 1 via doom_audio_worker(),
// registered as the native audio callback.  The worker uses wall-clock time
// (getTimeUs) to decide how many samples to produce each call, completely
// decoupled from the game's frame rate on Core 0.  Core 0 writes channel
// state (start/stop/params) with a per-channel sequence number; Core 1 only
// picks up new channel state when the seq changes, preserving its own
// playback positions between calls.

#include <string.h>
#include <stdlib.h>

#include "doomfeatures.h"

#ifdef FEATURE_SOUND

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "os.h"
#include "mus_player.h"

extern const PicoCalcAPI *g_picos_api;

// --- Output rate ---
#define MIX_RATE        11025
#define MIX_CHANNELS    8

// Chunk size for inner mix loop (reuses existing buffer)
#define MIX_BATCH       (MIX_RATE / 35)   // ~315
// Maximum samples per worker call (~100ms worth, safety cap)
#define MIX_BATCH_MAX   (MIX_RATE / 10)   // ~1102

#define MUS_RATE        140  // MUS sequencer ticks per second

// --- DMX lump header ---
#define DMX_MAGIC       0x0003
#define DMX_HDR_SIZE    8
#define DMX_PAD         16

// --- Cached sound data (stored in sfxinfo->driver_data) ---
typedef struct {
    const uint8_t *data;    // PCM samples (8-bit unsigned)
    uint32_t       length;  // number of samples
    uint32_t       rate;    // sample rate from DMX header
} cached_sfx_t;

// --- Per-channel mixer state ---
typedef struct {
    boolean         active;
    const uint8_t  *data;
    uint32_t        length;
    uint32_t        pos;      // 16.16 fixed-point position
    uint32_t        step;     // 16.16 fixed-point step
    int             left_vol;
    int             right_vol;
    uint32_t        seq;      // sequence number — incremented on each new sound start
} mix_channel_t;

static mix_channel_t s_channels[MIX_CHANNELS];

// Core 1 owns this snapshot — selectively updated via sequence numbers.
static mix_channel_t s_channels_snapshot[MIX_CHANNELS];

// Per-channel sequence tracking: Core 1's last-seen seq per channel
static uint32_t s_channel_seq[MIX_CHANNELS];
static uint32_t s_next_seq = 1;  // global monotonic counter

static int16_t s_mix_buf[MIX_BATCH * 2];  // stereo interleaved

// Time-based worker state
static uint32_t s_last_mix_us = 0;        // wall-clock time of last mix
static uint32_t s_mus_tick_accum = 0;     // fractional MUS tick accumulator (µs)

// --- Volume/pan helpers ---

static void compute_volumes(int vol, int sep, int *left, int *right)
{
    // sep: 0=hard left, 127=center, 254=hard right
    // vol: 0..127
    *left  = ((254 - sep) * vol) / 127;
    *right = (sep * vol) / 127;
}

// --- Core 1 audio worker ---
// Called every 5ms from core1_entry().  Produces audio autonomously based on
// wall-clock time, completely decoupled from the game's frame rate.
//
// IMPORTANT: mus_tick_n() and mus_render() MUST run on the same core because
// they both access the OPL emulator state (s_mus.opl).  mus_tick_n() writes
// OPL registers via OPL3_WriteRegBuffered, and mus_render() reads them via
// OPL3_GenerateBatch.  Running these on different cores corrupts the OPL
// state.  picos_mus_poll() is therefore a no-op; we call mus_tick_n() here.

static void mix_chunk(int n)
{
    int16_t *out = s_mix_buf;

    for (int i = 0; i < n; i++) {
        int32_t left = 0, right = 0;

        for (int ch = 0; ch < MIX_CHANNELS; ch++) {
            mix_channel_t *c = &s_channels_snapshot[ch];
            if (!c->active)
                continue;

            uint32_t idx = c->pos >> 16;
            if (idx >= c->length) {
                c->active = false;
                // Write back completion so Core 0 sees it
                s_channels[ch].active = false;
                continue;
            }

            // 8-bit unsigned → signed ~int16 range
            int32_t sample = ((int32_t)c->data[idx] - 128) << 8;
            left  += (sample * c->left_vol) >> 8;
            right += (sample * c->right_vol) >> 8;

            c->pos += c->step;
        }

        // Clamp to int16_t
        if (left > 32767) left = 32767;
        else if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        else if (right < -32768) right = -32768;

        *out++ = (int16_t)left;
        *out++ = (int16_t)right;
    }

    // Mix in music PCM if playing
    if (mus_is_playing()) {
        int16_t music_buf[MIX_BATCH * 2];
        mus_render(music_buf, n);
        for (int j = 0; j < n * 2; j++) {
            int32_t mixed = (int32_t)s_mix_buf[j] + (int32_t)music_buf[j];
            if (mixed > 32767) mixed = 32767;
            else if (mixed < -32768) mixed = -32768;
            s_mix_buf[j] = (int16_t)mixed;
        }
    }

    g_picos_api->audio->pushSamples(s_mix_buf, n);
}

static void doom_audio_worker(void)
{
    uint32_t now = (uint32_t)g_picos_api->sys->getTimeUs();

    // First call: seed the timestamp and return
    if (s_last_mix_us == 0) {
        s_last_mix_us = now;
        return;
    }

    uint32_t elapsed_us = now - s_last_mix_us;
    if (elapsed_us < 1000)  // <1ms, nothing to do
        return;

    // How many PCM samples to produce for this elapsed period
    uint32_t samples = (elapsed_us * MIX_RATE) / 1000000;
    if (samples == 0)
        return;
    if (samples > MIX_BATCH_MAX)
        samples = MIX_BATCH_MAX;

    s_last_mix_us = now;

    // Advance MUS sequencer based on elapsed time (140Hz ticks)
    s_mus_tick_accum += elapsed_us;
    uint32_t mus_ticks = (s_mus_tick_accum * MUS_RATE) / 1000000;
    if (mus_ticks > 0) {
        s_mus_tick_accum -= (mus_ticks * 1000000) / MUS_RATE;
        mus_tick_n(mus_ticks);
    }

    // Selective snapshot: only pick up channels that Core 0 has changed
    for (int ch = 0; ch < MIX_CHANNELS; ch++) {
        uint32_t seq = s_channels[ch].seq;
        if (seq != s_channel_seq[ch]) {
            // Core 0 started/stopped a new sound on this channel
            s_channels_snapshot[ch] = s_channels[ch];
            s_channel_seq[ch] = seq;
        } else if (s_channels_snapshot[ch].active) {
            // Pick up live volume changes without resetting position
            s_channels_snapshot[ch].left_vol = s_channels[ch].left_vol;
            s_channels_snapshot[ch].right_vol = s_channels[ch].right_vol;
        }
    }

    // Mix in chunks of MIX_BATCH (reuses s_mix_buf)
    while (samples > 0) {
        int n = (samples > MIX_BATCH) ? MIX_BATCH : (int)samples;
        mix_chunk(n);
        samples -= n;
    }
}

// --- Sound module callbacks ---

static boolean picos_snd_init(boolean use_sfx_prefix)
{
    memset(s_channels, 0, sizeof(s_channels));
    memset(s_channels_snapshot, 0, sizeof(s_channels_snapshot));
    memset(s_channel_seq, 0, sizeof(s_channel_seq));
    s_next_seq = 1;
    s_last_mix_us = 0;
    s_mus_tick_accum = 0;
    g_picos_api->audio->startStream(MIX_RATE);

    // Register our worker on Core 1
    g_picos_api->sys->setAudioCallback(doom_audio_worker);

    return true;
}

static void picos_snd_shutdown(void)
{
    g_picos_api->sys->setAudioCallback(NULL);
    s_last_mix_us = 0;
    g_picos_api->audio->stopStream();
}

static int picos_snd_get_sfx_lump_num(sfxinfo_t *sfxinfo)
{
    char namebuf[16];
    snprintf(namebuf, sizeof(namebuf), "ds%s", sfxinfo->name);

    // W_GetNumForName calls I_Error if not found; use W_CheckNumForName
    // which returns -1 on miss.
    int lump = W_CheckNumForName(namebuf);
    return lump;
}

// Called on Core 0 every game tick (35Hz).  No-op: audio production is
// autonomous on Core 1 via doom_audio_worker(), driven by wall-clock time.
static void picos_snd_update(void)
{
    // Nothing to do — Core 1 snapshots channels and mixes autonomously.
}

static void picos_snd_update_params(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= MIX_CHANNELS)
        return;
    compute_volumes(vol, sep,
                    &s_channels[channel].left_vol,
                    &s_channels[channel].right_vol);
}

static int picos_snd_start(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= MIX_CHANNELS)
        return -1;

    // Lazy-cache the DMX lump data
    if (sfxinfo->driver_data == NULL) {
        int lump = sfxinfo->lumpnum;
        if (lump < 0)
            return -1;

        int lump_len = W_LumpLength(lump);
        if (lump_len < (int)(DMX_HDR_SIZE + DMX_PAD))
            return -1;

        uint8_t *raw = W_CacheLumpNum(lump, PU_STATIC);
        if (!raw)
            return -1;

        // Validate DMX magic
        uint16_t magic = raw[0] | (raw[1] << 8);
        if (magic != DMX_MAGIC) {
            W_ReleaseLumpNum(lump);
            return -1;
        }

        uint16_t rate = raw[2] | (raw[3] << 8);
        uint32_t num_samples = raw[4] | (raw[5] << 8) |
                               (raw[6] << 16) | (raw[7] << 24);

        // Bounds check: DMX length field includes the 16-byte padding
        // at start and 16 bytes at end
        if (num_samples < DMX_PAD * 2)
            num_samples = 0;
        else
            num_samples -= DMX_PAD * 2;

        uint32_t data_available = lump_len - DMX_HDR_SIZE - DMX_PAD;
        if (num_samples > data_available)
            num_samples = data_available;

        cached_sfx_t *cache = malloc(sizeof(cached_sfx_t));
        if (!cache) {
            W_ReleaseLumpNum(lump);
            return -1;
        }

        cache->data   = raw + DMX_HDR_SIZE + DMX_PAD;
        cache->length = num_samples;
        cache->rate   = rate ? rate : 11025;

        sfxinfo->driver_data = cache;
    }

    cached_sfx_t *sfx = (cached_sfx_t *)sfxinfo->driver_data;
    if (sfx->length == 0)
        return -1;

    mix_channel_t *c = &s_channels[channel];
    c->active   = true;
    c->data     = sfx->data;
    c->length   = sfx->length;
    c->pos      = 0;
    c->step     = (sfx->rate << 16) / MIX_RATE;

    compute_volumes(vol, sep, &c->left_vol, &c->right_vol);

    c->seq = s_next_seq++;

    return channel;
}

static void picos_snd_stop(int channel)
{
    if (channel >= 0 && channel < MIX_CHANNELS) {
        s_channels[channel].active = false;
        s_channels[channel].seq = s_next_seq++;
    }
}

static boolean picos_snd_is_playing(int channel)
{
    if (channel >= 0 && channel < MIX_CHANNELS)
        return s_channels[channel].active;
    return false;
}

static void picos_snd_cache_sounds(sfxinfo_t *sounds, int num_sounds)
{
    // Lazy cache in StartSound — no-op here
}

// --- Sound module definition ---

static snddevice_t picos_snd_devices[] = { SNDDEVICE_SB };

sound_module_t DG_sound_module = {
    picos_snd_devices,
    sizeof(picos_snd_devices) / sizeof(*picos_snd_devices),
    picos_snd_init,
    picos_snd_shutdown,
    picos_snd_get_sfx_lump_num,
    picos_snd_update,
    picos_snd_update_params,
    picos_snd_start,
    picos_snd_stop,
    picos_snd_is_playing,
    picos_snd_cache_sounds,
};

// --- Music module (OPL2 via Nuked-OPL3) ---

static boolean picos_mus_init(void)
{
    mus_init();
    return true;
}

static void picos_mus_shutdown(void)
{
    mus_shutdown();
}

static void picos_mus_set_volume(int volume)
{
    mus_set_volume(volume);
}

static void picos_mus_pause(void)
{
    mus_pause();
}

static void picos_mus_resume(void)
{
    mus_resume();
}

static void *picos_mus_register(void *data, int len)
{
    return mus_register(data, len);
}

static void picos_mus_unregister(void *handle)
{
    mus_unregister((mus_song_t *)handle);
}

static void picos_mus_play(void *handle, boolean looping)
{
    mus_play((mus_song_t *)handle, looping);
}

static void picos_mus_stop(void)
{
    mus_stop();
}

static boolean picos_mus_is_playing(void)
{
    return mus_is_playing();
}

static void picos_mus_poll(void)
{
    // No-op: mus_tick() is called from doom_audio_worker() on Core 1
    // to keep all OPL access on a single core (see comment above worker).
}

static snddevice_t picos_mus_devices[] = { SNDDEVICE_SB, SNDDEVICE_ADLIB, SNDDEVICE_GENMIDI };

music_module_t DG_music_module = {
    picos_mus_devices,
    sizeof(picos_mus_devices) / sizeof(*picos_mus_devices),
    picos_mus_init,
    picos_mus_shutdown,
    picos_mus_set_volume,
    picos_mus_pause,
    picos_mus_resume,
    picos_mus_register,
    picos_mus_unregister,
    picos_mus_play,
    picos_mus_stop,
    picos_mus_is_playing,
    picos_mus_poll,
};

// --- Required globals for I_BindSoundVariables ---
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

// --- Required by i_sound.h when FEATURE_SOUND is on (normally in i_sdlmusic.c / dummy.c) ---
void I_InitTimidityConfig(void) {}

#endif /* FEATURE_SOUND */
