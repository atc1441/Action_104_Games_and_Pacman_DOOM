#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

/* Rebuild of the stock firmware's SystemInit: I-cache, FPU, oscillator.
 * Must be the first thing main() calls. */
void clock_init(void);

/* Core clock in Hz, set by clock_init(). */
extern uint32_t g_cpu_hz;

/* Step up to the ~194 MHz PLL. MPI retune runs from SRAM; OSC bit 3,
 * PLL lock, source switch and R20 tail run from flash, as in stock
 * app_board_init. Returns 0 on success, <0 if the PLL did not lock. */
int clock_boost(void);

#endif /* CLOCK_H */
