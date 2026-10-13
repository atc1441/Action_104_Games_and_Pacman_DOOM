#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/*
 * Audio output driver.
 *
 * Streams unsigned samples out of two ping-pong buffers through DMA
 * channel 0 into TIM1_CCR1, the duty cycle of the PWM timer at
 * 0x40012C00, which is exactly what the stock firmware does. See
 * docs/HARDWARE.md for how that was worked out.
 */

/* Samples per DMA buffer. Live Forest Kid: ping-pong stride is 0x5CA
 * (741 halfwords) but the descriptor reload count is 0x2E1 = 737. */
#define AUDIO_BUF_SAMPLES 737

/* Bring TIM1 and DMA up and start streaming. Safe to call once. From
 * then on the SysTick interrupt refills the buffers. */
void audio_init(void);

/*
 * Supplied by the mixer (port/i_sound_console.c). Fills n samples.
 * Runs in the SysTick interrupt.
 *
 * Values are unsigned, 0..255 with 128 as silence, which is what the
 * stock firmware feeds TIM1. 255 is TIM1's ARR, so that is the full
 * duty-cycle range; larger values are outside it and have not been
 * tested.
 */
void audio_fill(uint16_t *dst, unsigned n);

/* Output sample rate in Hz, valid after audio_init(): ~44.1 kHz, the
 * exact value depends on the core clock. */
uint32_t audio_rate(void);

/* Buffers completed since audio_init(). Multiply by AUDIO_BUF_SAMPLES and
 * divide by elapsed seconds to measure the output rate on the device. */
uint32_t audio_blocks(void);

#endif /* AUDIO_H */
