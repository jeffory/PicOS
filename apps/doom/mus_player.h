// MUS music player for PicOS DOOM
// MUS sequencer + OPL2 synthesizer using Nuked-OPL3 in OPL2 mode
//
// Architecture:
//   WAD MUS lump -> MUS sequencer (mus_tick) -> OPL register writes
//   -> Nuked-OPL3 emulator -> PCM (mus_render)
//
// Call pattern:
//   mus_tick()   - called from music_module->Poll() (after pushSamples)
//   mus_render() - called from picos_snd_update() (before pushSamples)

#ifndef MUS_PLAYER_H
#define MUS_PLAYER_H

#include <stdint.h>

// Opaque song handle
typedef struct mus_song mus_song_t;

// Initialize OPL and load GENMIDI lump from WAD
void        mus_init(void);

// Shutdown and free resources
void        mus_shutdown(void);

// Register a MUS lump (returns handle for play/unregister)
mus_song_t *mus_register(const void *data, int len);

// Free a registered song
void        mus_unregister(mus_song_t *song);

// Start playing a song (looping: nonzero = loop)
void        mus_play(mus_song_t *song, int looping);

// Stop playback
void        mus_stop(void);

// Pause/resume
void        mus_pause(void);
void        mus_resume(void);

// Set master volume (0-127, as per DOOM's music volume)
void        mus_set_volume(int volume);

// Is music currently playing? (returns nonzero if yes)
int         mus_is_playing(void);

// Advance the MUS sequencer (called from Poll, 35Hz game tick)
// Internally runs 4 MUS ticks per call (MUS rate = 140Hz)
void        mus_tick(void);

// Render PCM samples into buffer (stereo interleaved int16_t)
// count = number of stereo sample pairs
// Called from picos_snd_update() before pushSamples
void        mus_render(int16_t *buf, int count);

#endif // MUS_PLAYER_H
