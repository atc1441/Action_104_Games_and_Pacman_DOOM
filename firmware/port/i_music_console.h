#ifndef I_MUSIC_CONSOLE_H
#define I_MUSIC_CONSOLE_H

#include <stdint.h>

/* rate: the rate I_MusicRender produces samples at. Needs the WAD. */
void I_MusicInit(uint32_t rate);

/* Adds n music samples into out; called from the audio interrupt. */
void I_MusicRender(int32_t *out, unsigned n);

#endif /* I_MUSIC_CONSOLE_H */
