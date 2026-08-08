#ifndef DOOM_IWAD_H
#define DOOM_IWAD_H

/* Pointer instead of array: the WAD lives in XIP flash, not in the image. */
extern const unsigned char *const doom_iwad;
extern const unsigned int doom_iwad_len;

#endif // DOOM_IWAD_H
