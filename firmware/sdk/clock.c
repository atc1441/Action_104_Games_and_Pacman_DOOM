/*
 * clock.c - clock, cache and FPU init
 *
 * A 1:1 rebuild of the stock firmware's SystemInit
 * (flash 0x08025354 - 0x0802549C), which its reset handler calls at
 * 0x08004118 before anything else.
 *
 * Deliberately copied rather than "understood": without a datasheet the
 * meaning of individual bits in 0x4002103C / 0x40021020 / 0x40021048
 * cannot be pinned down, while the order is unambiguous from the code.
 * What the sequence actually achieves was measured, not guessed.
 *
 * The bootloader hands over at 16 MHz. Everything above that happens here.
 */

#include "clock.h"
#include "soc.h"
#include "hostbox.h"

/* Cortex-M system registers */
#define SCB_CCR      REG32(0xE000ED14u)   /* configuration and control     */
#define SCB_CPACR    REG32(0xE000ED88u)   /* coprocessor access control    */
#define SCB_ICIALLU  REG32(0xE000EF50u)   /* I-cache invalidate all        */

#define CCR_IC       (1u << 17)           /* instruction cache enable      */
#define CPACR_FPU    (0xFu << 20)         /* CP10/CP11 full access         */

/*
 * Actual core clock after clock_init(). Anything that measures time needs
 * it - a constant hard-coded at the call site breaks the moment the PLL
 * comes into play.
 */
uint32_t g_cpu_hz = 16000000u;

#define CPU_HZ_BASE   61200000u    /* measured after clock_init()          */
#define CPU_HZ_PLL    194063312u   /* Fref 15,525,065 Hz * 50 / 4          */

/*
 * The RCC registers SystemInit touches. The stock code addresses them
 * relative to r1 = 0x4002103C.
 */
#define RCC_OSC      REG32(RCC_BASE + 0x3C)   /* bit0 = on, bit4 = ready   */
#define RCC_R1C      REG32(RCC_BASE + 0x1C)
#define RCC_R20      REG32(RCC_BASE + 0x20)   /* bit31 = ready             */
#define RCC_R48      REG32(RCC_BASE + 0x48)
#define RCC_PLLCFG   REG32(RCC_BASE + 0x4C)   /* PLL M/P/Q                 */

#define OSC_ON       (1u << 0)
#define OSC_BIT3     (1u << 3)
#define OSC_READY    (1u << 4)
#define R20_READY    (1u << 31)

#define DSB() __asm volatile ("dsb sy" ::: "memory")
#define ISB() __asm volatile ("isb sy" ::: "memory")

/* The stock code simply counts down from 3 here. */
static inline void short_delay(void)
{
    for (volatile uint32_t i = 3; i; i--) { }
}

void clock_init(void)
{
    /* --- I-cache: invalidate, then enable --- */
    DSB(); ISB();
    SCB_ICIALLU = 0;
    DSB(); ISB();
    SCB_CCR |= CCR_IC;
    DSB(); ISB();

    /* --- enable the FPU ---
     * The stock code does this here, so an FPU is fitted. The build still
     * uses -mfloat-abi=soft; this only grants access, it does not use it. */
    SCB_CPACR |= CPACR_FPU;

    /* --- oscillator on, wait for ready --- */
    RCC_OSC |= OSC_ON;
    short_delay();
    while (!(RCC_OSC & OSC_READY)) { }

    RCC_R1C = 0;
    RCC_R20 = 0;
    short_delay();
    while (!(RCC_R20 & R20_READY)) { }

    RCC_R48 &= ~1u;
    RCC_R48 |=  0x00200000u;

    /* Clear bit 3 and wait for ready again. As far as the measurements
     * show, this is the actual switch-over. */
    RCC_OSC &= ~OSC_BIT3;
    short_delay();
    while (!(RCC_OSC & OSC_READY)) { }

    g_cpu_hz = CPU_HZ_BASE;

    /* --- optional: step up to the PLL ---
     * Off by default and for a good reason; read clock_boost() below
     * before switching it on. */
#ifdef ENABLE_CLOCK_BOOST
    if (clock_boost() == 0) {
        g_cpu_hz = CPU_HZ_PLL;
    }
#endif

    /*
     * At this point the stock code sets VTOR = 0x08004000. We deliberately
     * do not: the startup code already pointed VTOR at our own vector
     * table, which for a RAM image sits somewhere else entirely.
     */
}

/*
 * ============================ CLOCK TREE ============================
 *
 * From the stock firmware's SystemCoreClock routine (0x08005588):
 *
 *   source:      RCC +0x1C bits[2:0]   0/1 reference, 2 = 12 MHz, 3 = PLL
 *   PLL:         F = Fref * (M + 12) / (P + 1) / (Q + 1)
 *                  M = +0x4C bits[5:0], P = bits[10:8], Q = bits[15:12]
 *   post-divide: +0x20 bits[3:0] and [7:4], each +1
 *   Fref:        (0x600000B0 >> 2), valid when (0x60000080 & 0xFE00) == 0
 *                and the upper half of 0x60000080 is the one's complement
 *                of the lower half; otherwise 16 MHz.
 *
 * On this device: Fref = 15,525,065 Hz, +0x4C = 0x326 (M=38, P=3, Q=0)
 *   => 15,525,065 * 50 / 4 = 194.06 MHz
 *
 * ===================== WHY THIS IS OFF BY DEFAULT ====================
 *
 * The register sequence below is complete and correct - it was read
 * straight out of app_board_init (0x0802AD96 ff.) of the stock firmware.
 * It still hangs the SoC, and the reason is not the sequence:
 *
 * This code executes via XIP from the external SPI flash. The moment the
 * core clock jumps, the flash interface timing no longer holds, and the
 * very next instruction fetch fails. The symptom is distinctive: current
 * draw rises from 61 mA to 124 mA, the display stays dark, and the debug
 * port is gone. The PLL is running fine - the core simply cannot read
 * another instruction.
 *
 * The stock firmware avoids this the only way that works: before touching
 * the flash controller it copies a small routine into RAM (0x0802AA70 -
 * the stored constants 0xF8413A01 / 0xD1F90F04 are Thumb code, not data)
 * and calls it with interrupts disabled.
 *
 * To finish this: put clock_boost() into a .ramfunc section, copy it to
 * RAM at startup, call it with interrupts masked, and adjust the MPI
 * divider along with the core clock. Until then the console runs at
 * ~62 MHz, which is enough for DOOM at a playable frame rate.
 *
 * If you enable it anyway and the console stops responding, see
 * docs/FLASHING.md - tools/rescue.py gets it back.
 */
#define R48_PLL_ON    (1u << 0)
#define R48_BIT21     (1u << 21)
#define R48_PLL_EN    (1u << 22)      /* enable the PLL itself */
#define R48_LOCK30    (1u << 30)
#define R48_BIT1      (1u << 1)

#define SRC_PLL       3u
#define PLLCFG_194MHZ 0x326u          /* M=38, P=3, Q=0 */

/* Progress markers, survive a warm reset - without them there is no way
 * to tell "PLL never locked" from "the switch killed it". */
#define MARK  g_hostbox.mark
#define MARKV g_hostbox.markv

int clock_boost(void)
{
    MARK = 1; MARKV = RCC_R48;

    /* reference on */
    RCC_R48 |= R48_PLL_ON;
    RCC_R48 &= ~R48_BIT21;
    MARK = 2; MARKV = RCC_R48;
    short_delay();

    /* first lock - still only the reference, not the PLL */
    MARK = 3;
    for (uint32_t t = 2000000u; !(RCC_R48 & R48_LOCK30); t--) {
        if (!t) { MARK = 0x81; MARKV = RCC_R48; return -1; }
    }

    /* Set the dividers and only NOW switch the PLL itself on. These two
     * steps were missing from earlier attempts, which switched the clock
     * source to a PLL that had never been enabled. */
    MARK = 4;
    RCC_R48 &= ~R48_BIT1;
    RCC_PLLCFG = PLLCFG_194MHZ;
    RCC_R48 |= R48_PLL_EN;
    MARK = 5; MARKV = RCC_R48;
    short_delay();

    /* second lock - now the PLL */
    MARK = 6;
    for (uint32_t t = 2000000u; !(RCC_R48 & R48_LOCK30); t--) {
        if (!t) { MARK = 0x82; MARKV = RCC_R48; return -2; }
    }

    /* Sanity check as in the original - which bits apply depends on bit 1. */
    const uint32_t r48  = RCC_R48;
    const uint32_t mask = (r48 & R48_BIT1) ? 0x20200001u : 0x40200001u;
    const uint32_t want = (r48 & R48_BIT1) ? 0x20000001u : 0x40000001u;
    MARK = 7; MARKV = r48;
    if ((r48 & mask) != want) { MARK = 0x83; return -3; }

    /* only now switch the source */
    MARK = 8;
    RCC_R1C = (RCC_R1C & ~7u) | SRC_PLL;
    RCC_R20 &= ~0xFu;                       /* post-divider to 1 */
    short_delay();
    MARK = 9; MARKV = RCC_R48;
    return 0;
}
