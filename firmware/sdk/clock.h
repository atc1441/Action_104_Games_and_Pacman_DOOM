#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/* Rebuild of the stock firmware's SystemInit: I-cache, FPU, oscillator.
 * Must be the first thing main() calls. */
void clock_init(void);

/* Core clock in Hz, set by clock_init(). */
extern uint32_t g_cpu_hz;

/* Step up to the ~194 MHz PLL. Returns 0 on success, <0 if the PLL did
 * not lock. Off by default - read the long comment in clock.c before
 * enabling ENABLE_CLOCK_BOOST; it hangs the SoC for a reason that has
 * nothing to do with the register sequence. */
int clock_boost(void);

#endif /* CLOCK_H */
