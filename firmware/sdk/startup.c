/*
 * startup.c - vector table and reset handler
 *
 * Note what this does NOT touch: the clock configuration and the flash
 * controller (0x52005000). The bootloader has already set up both, and
 * this code runs via XIP through exactly that controller - reconfiguring
 * it while executing from it is the fastest route to a hung system.
 */

#include <stdint.h>
#include "soc.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

int  main(void);
void Reset_Handler(void);
void Default_Handler(void);

/*
 * Every exception and IRQ lands here initially.
 *
 * Deliberately a plain spin loop rather than "wfi": if the core sleeps
 * here, this SoC powers the debug domain down and J-Link only reports
 * "Failed to attach to CPU". A crash would then be unreachable except by
 * power cycling, and impossible to investigate. This way the core stays
 * halt-able and the fault site can be found from the stack.
 */
void Default_Handler(void)
{
    for (;;) { }
}

#define ALIAS(f) __attribute__((weak, alias(#f)))

void NMI_Handler(void)        ALIAS(Default_Handler);
void HardFault_Handler(void)  ALIAS(Default_Handler);
void MemManage_Handler(void)  ALIAS(Default_Handler);
void BusFault_Handler(void)   ALIAS(Default_Handler);
void UsageFault_Handler(void) ALIAS(Default_Handler);
void SVC_Handler(void)        ALIAS(Default_Handler);
void DebugMon_Handler(void)   ALIAS(Default_Handler);
void PendSV_Handler(void)     ALIAS(Default_Handler);
void SysTick_Handler(void)    ALIAS(Default_Handler);

/*
 * 16 system vectors + 40 IRQs.
 * The 40 comes from the stock bootloader's vector table: from index 56
 * (offset 0xE0) onwards it already contains code.
 */
#define NUM_IRQ 40

__attribute__((section(".isr_vector"), used))
void (* const g_vectors[16 + NUM_IRQ])(void) = {
    (void (*)(void))&_estack,   /*  0  initial SP          */
    Reset_Handler,              /*  1  reset               */
    NMI_Handler,                /*  2                      */
    HardFault_Handler,          /*  3                      */
    MemManage_Handler,          /*  4                      */
    BusFault_Handler,           /*  5                      */
    UsageFault_Handler,         /*  6                      */
    0, 0, 0, 0,                 /*  7-10 reserved          */
    SVC_Handler,                /* 11                      */
    DebugMon_Handler,           /* 12                      */
    0,                          /* 13 reserved             */
    PendSV_Handler,             /* 14                      */
    SysTick_Handler,            /* 15                      */

    /* 40 peripheral IRQs - all to Default_Handler */
    [16 ... 16 + NUM_IRQ - 1] = Default_Handler,
};

void Reset_Handler(void)
{
    uint32_t *src, *dst;

    /* copy .data from flash into RAM (a no-op for a RAM image) */
    src = &_sidata;
    dst = &_sdata;
    if (src != dst) {
        while (dst < &_edata) { *dst++ = *src++; }
    }

    /* zero .bss */
    for (dst = &_sbss; dst < &_ebss; ) { *dst++ = 0; }

    /* Announce the vector table. The bootloader already set VTOR to
     * 0x08004000; for a RAM image it has to be corrected here. */
    SCB_VTOR = (uint32_t)(uintptr_t)g_vectors;

    /* Enable the cycle counter - used as a time base and for measuring
     * the actual core clock from the host. */
    DEMCR    |= (1u << 24);   /* TRCENA    */
    DWT_CYCCNT = 0;
    DWT_CTRL |= 1u;           /* CYCCNTENA */

    (void)main();

    for (;;) { }
}
