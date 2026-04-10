/*
  DOS86 — Audio mixer.

  Combines PC speaker (square wave) and OPL2 (FM synthesis) into a stereo
  PCM stream pushed to PicOS audio via Core 1 callback.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.
*/

#ifndef DOS86_AUDIO_MIX_H
#define DOS86_AUDIO_MIX_H

struct PicoCalcAPI;

/* Initialize the audio mixer and start PCM streaming.
   Must be called from Core 0 after api is available. */
void audio_mix_init(const struct PicoCalcAPI *api);

/* Stop PCM streaming and deregister the Core 1 callback. */
void audio_mix_shutdown(void);

#endif /* DOS86_AUDIO_MIX_H */
