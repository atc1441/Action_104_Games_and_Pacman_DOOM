#ifndef OPL2_H
#define OPL2_H

#include <stdint.h>

/*
 * A small software OPL2 (YM3812): nine two-operator FM channels driven
 * through the chip's own registers, rendered as mono samples. See opl2.c
 * for what it leaves out.
 *
 * OPL_CHANNELS 18 adds a second set of nine, reached as on an OPL3 with
 * register numbers 0x100 higher; each set still behaves as an OPL2.
 */
#ifndef OPL_CHANNELS
#define OPL_CHANNELS 9
#endif

/* Clears every register; rate is the output sample rate in Hz. */
void opl_init(uint32_t rate);

/* Writes one chip register, as an OUT to the real chip's ports would;
 * 0x100 and up is the second set of channels. */
void opl_write(unsigned reg, unsigned val);

/* Adds n samples of all channels into out, each channel within +-4095
 * at full level. */
void opl_render(int32_t *out, unsigned n);

#endif /* OPL2_H */
