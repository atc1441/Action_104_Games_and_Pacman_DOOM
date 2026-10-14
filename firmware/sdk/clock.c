/*
 * clock.c - clock, cache and FPU init
 *
 * A 1:1 rebuild of the stock firmware's SystemInit, which its reset
 * handler calls before anything else.
 *
 * Deliberately copied rather than "understood": without a datasheet the
 * meaning of individual bits in 0x4002103C / 0x40021020 / 0x40021048
 * cannot be pinned down, while the order is unambiguous from the code.
 * The resulting clock was measured.
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
#define SCB_CCSIDR   REG32(0xE000ED80u)   /* cache size ID                 */
#define SCB_CSSELR   REG32(0xE000ED84u)   /* cache size selection          */
#define SCB_DCISW    REG32(0xE000EF60u)   /* D-cache invalidate by set/way */

#define CCR_IC       (1u << 17)           /* instruction cache enable      */
#define CCR_DC       (1u << 16)           /* data cache enable             */

/* ARMv8-M MPU */
#define MPU_CTRL     REG32(0xE000ED94u)
#define MPU_RNR      REG32(0xE000ED98u)
#define MPU_RBAR     REG32(0xE000ED9Cu)
#define MPU_RLAR     REG32(0xE000EDA0u)
#define MPU_MAIR0    REG32(0xE000EDC0u)
#define CPACR_FPU    (0xFu << 20)         /* CP10/CP11 full access         */

/*
 * Actual core clock after clock_init(). Anything that measures time needs
 * it - a constant hard-coded at the call site breaks the moment the PLL
 * comes into play.
 */
uint32_t g_cpu_hz = 16000000u;

#define CPU_HZ_BASE   62200000u    /* measured after clock_init()          */
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

#define RAMFUNC __attribute__((section(".ramfunc"), noinline, noclone))

static void dcache_init(void);

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

    /* --- step up to the PLL. Sequence matches stock app_board_init;
     * boot with the probe disconnected across the source switch. */
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

    dcache_init();
}

/*
 * D-cache, 8 KB (CCSIDR: 64 sets, 4 ways, 32-byte lines). The flash sits
 * in the Code region, which the default memory map makes cacheable
 * write-through, so the WAD data read through XIP gets cached.
 *
 * SRAM would default to write-back, and the DMA reads the audio and pixel
 * buffers from SRAM behind the cache's back. Two MPU regions keep SRAM
 * coherent without any cache maintenance in the drivers:
 *
 *   0  0x20000000-0x200000FF  non-cacheable  hostbox, written over SWD
 *   1  0x20000100-0x2007FFFF  write-through  everything else
 *
 * Everything outside them keeps the default map (PRIVDEFENA).
 */
#define MAIR_NC      0x44u                /* normal, non-cacheable         */
#define MAIR_WT      0xAAu                /* normal, write-through, RA     */
#define RBAR_AP_RW   (1u << 1)            /* read/write, any privilege     */

static void dcache_init(void)
{
    DSB();
    MPU_CTRL  = 0;
    MPU_MAIR0 = MAIR_NC | (MAIR_WT << 8);

    MPU_RNR  = 0;
    MPU_RBAR = HOSTBOX_ADDR | RBAR_AP_RW;
    MPU_RLAR = ((HOSTBOX_ADDR + 0x100u - 1u) & ~0x1Fu) | (0u << 1) | 1u;

    MPU_RNR  = 1;
    MPU_RBAR = (HOSTBOX_ADDR + 0x100u) | RBAR_AP_RW;
    MPU_RLAR = (0x2007FFFFu & ~0x1Fu) | (1u << 1) | 1u;

    MPU_CTRL = (1u << 2) | 1u;            /* PRIVDEFENA | ENABLE           */
    DSB(); ISB();

    /* Invalidate by set/way, then enable. */
    SCB_CSSELR = 0;                       /* level 1 data cache            */
    DSB();
    const uint32_t ccsidr = SCB_CCSIDR;
    const uint32_t sets = (ccsidr >> 13) & 0x7FFFu;   /* minus one */
    const uint32_t ways = (ccsidr >> 3) & 0x3FFu;     /* minus one */
    for (uint32_t s = 0; s <= sets; s++)
        for (uint32_t w = 0; w <= ways; w++)
            SCB_DCISW = (s << 5) | (w << 30);
    DSB();

    SCB_CCR |= CCR_DC;
    DSB(); ISB();
}

/*
 * ============================ CLOCK TREE ============================
 *
 * From the stock firmware's SystemCoreClock routine:
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
 * ===================== CLOCK BOOST (stock app_board_init) ====================
 *
 * clock_boost() matches stock app_board_init:
 * a .ramfunc ORs 0x100 into MPI +0x10 (and keeps divider 2), then the
 * rest runs from flash — the same split as stock. OSC bit 3 is set again
 * before lock (SystemInit cleared it for 62 MHz). After source=3, R20
 * bits[3:0] and [7:4] are cleared with a ready wait each, then bits[13:8]
 * are set to 0x24 (live stock R20 = 0x80002400). Do not attach SWD across
 * the source switch; it desyncs MEM-AP. Flash with --leave-halted and
 * power-cycle. If the console stops responding, see docs/FLASHING.md.
 */
#define R48_PLL_ON    (1u << 0)
#define R48_BIT21     (1u << 21)
#define R48_PLL_EN    (1u << 22)      /* enable the PLL itself */
#define R48_LOCK30    (1u << 30)
#define R48_BIT1      (1u << 1)

#define SRC_PLL       3u
#define PLLCFG_194MHZ 0x326u          /* M=38, P=3, Q=0 */
#define DBG_C4        REG32(0x400070C4u)  /* stock bic #0x100000 after PLL */

/* Progress markers, survive a warm reset - without them there is no way
 * to tell "PLL never locked" from "the switch killed it". */
#define MARK  g_hostbox.mark
#define MARKV g_hostbox.markv

RAMFUNC static void mpi_retune_for_pll(void)
{
    /* The stock SRAM stub: OR 0x100 into +0x10. Must not run from XIP. */
    uint32_t div = SPI_FLASH_DIV;
    SPI_FLASH_DIV = (div & 0xFFFF0000u) | 2u;
    SPI_FLASH_RXCTL |= 0x100u;
    DSB();
}

static int wait_r20_ready(void)
{
    for (uint32_t t = 2000000u; t; t--) {
        if (RCC_R20 & R20_READY) {
            return 0;
        }
    }
    return -1;
}

int clock_boost(void)
{
    MARK = 1; MARKV = RCC_R48;

    /* MPI first, while still at 62 MHz, matching stock's RAM stub then PLL. */
    __asm volatile ("cpsid i" ::: "memory");
    mpi_retune_for_pll();

    /* Stock app_board_init: (OSC & ~8) | 9, wait bit 4. SystemInit cleared
     * bit 3 for the 62 MHz switch; PLL lock is gated on (OSC & 0x19)==0x19. */
    MARK = 2;
    RCC_OSC = (RCC_OSC & ~OSC_BIT3) | OSC_ON | OSC_BIT3;
    short_delay();
    for (uint32_t t = 2000000u; !(RCC_OSC & OSC_READY); t--) {
        if (!t) { MARK = 0x80; MARKV = RCC_OSC; __asm volatile ("cpsie i" ::: "memory"); return -1; }
    }

    /* reference on */
    RCC_R48 |= R48_PLL_ON;
    RCC_R48 &= ~R48_BIT21;
    MARK = 3; MARKV = RCC_R48;
    short_delay();

    /* first lock - still only the reference, not the PLL */
    MARK = 4;
    for (uint32_t t = 2000000u; !(RCC_R48 & R48_LOCK30); t--) {
        if (!t) { MARK = 0x81; MARKV = RCC_R48; __asm volatile ("cpsie i" ::: "memory"); return -2; }
    }

    /* Set the dividers and only then switch the PLL itself on. */
    MARK = 5;
    RCC_R48 &= ~R48_BIT1;
    RCC_PLLCFG = PLLCFG_194MHZ;
    RCC_R48 |= R48_PLL_EN;
    MARK = 6; MARKV = RCC_R48;
    short_delay();

    /* second lock - now the PLL */
    MARK = 7;
    for (uint32_t t = 2000000u; !(RCC_R48 & R48_LOCK30); t--) {
        if (!t) { MARK = 0x82; MARKV = RCC_R48; __asm volatile ("cpsie i" ::: "memory"); return -3; }
    }

    /* Sanity check as in the original - which bits apply depends on bit 1. */
    const uint32_t r48  = RCC_R48;
    const uint32_t mask = (r48 & R48_BIT1) ? 0x20200001u : 0x40200001u;
    const uint32_t want = (r48 & R48_BIT1) ? 0x20000001u : 0x40000001u;
    MARKV = r48;
    if ((r48 & mask) != want) { MARK = 0x83; __asm volatile ("cpsie i" ::: "memory"); return -4; }

    MARK = 8;
    RCC_R1C = (RCC_R1C & ~7u) | SRC_PLL;
    RCC_R20 &= ~0xFu;
    if (wait_r20_ready()) { MARK = 0x84; MARKV = RCC_R20; __asm volatile ("cpsie i" ::: "memory"); return -5; }
    RCC_R20 &= ~0xF0u;
    if (wait_r20_ready()) { MARK = 0x85; MARKV = RCC_R20; __asm volatile ("cpsie i" ::: "memory"); return -6; }
    /* R20 bits 13:8 = 0x24, as the stock firmware sets them */
    RCC_R20 = (RCC_R20 & ~0x3F00u) | 0x2400u;
    DBG_C4 &= ~0x100000u;
    DSB(); ISB();
    SCB_ICIALLU = 0;
    DSB(); ISB();
    short_delay();
    __asm volatile ("cpsie i" ::: "memory");
    MARK = 9; MARKV = RCC_R48;
    return 0;
}
