/*
 * WAD access straight out of flash.
 *
 * Upstream GBADoom compiles the WAD into the image as a 3.8 MB array.
 * Here it lives in the external flash instead and is read through the XIP
 * window by pointer - w_wad.c does nothing but &doom_iwad[filepos]
 * anyway, exactly as the GBA reads from cartridge ROM.
 *
 * That saves the entire RAM and image budget for the WAD data. The WAD is
 * flashed separately to WAD_BASE; see docs/FLASHING.md.
 */
#include "doom_iwad.h"

/*
 * A full megabyte is reserved for the firmware even though the image is
 * about 545 KB, so there is room to grow without moving the WAD.
 */
#define WAD_BASE  0x08090000u     /* after firmware; room for DS* lumps  */
#define WAD_MAX   0x00370000u     /* rest of the 4 MB flash              */

const unsigned char *const doom_iwad = (const unsigned char *)WAD_BASE;
const unsigned int doom_iwad_len = WAD_MAX;
