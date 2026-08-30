#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/*
 * Audio output driver.
 *
 * Streams unsigned samples out of two ping-pong buffers through DMA
 * channel 0 into the DAC at 0x40012C00, which is exactly what the stock
 * firmware does. See docs/HARDWARE.md for how that was worked out.
 */

/* Samples per DMA buffer. Live Forest Kid: ping-pong stride is 0x5CA
 * (741 halfwords) but the descriptor reload count is 0x2E1 = 737. */
#define AUDIO_BUF_SAMPLES 737

/* Bring the DAC and DMA up and start streaming. Safe to call once. */
void audio_init(void);

/* Hand the idle buffer to the mixer if the DMA has finished with it.
 * Must be called far more often than once per buffer - see the comment in
 * audio.c about where it is called from. Cheap when there is nothing to
 * do: one register read. */
void audio_service(void);

/*
 * Supplied by the mixer (port/i_sound_console.c). Fills n samples.
 *
 * Values are unsigned, nominally 0..255 with 128 as silence, which is
 * what the stock firmware feeds the DAC. Anything beyond that range has
 * not been tested and may just wrap.
 */
void audio_fill(uint16_t *dst, unsigned n);

/* Buffers completed since audio_init(). Multiply by AUDIO_BUF_SAMPLES and
 * divide by elapsed seconds to measure the real output rate - it is not
 * known from the register map, only by measurement. */
uint32_t audio_blocks(void);

#endif /* AUDIO_H */
