/*
 * audio.c - TIM1 PWM output driven by DMA
 *
 * Rebuilt from the stock firmware, which was read live over SWD rather
 * than derived from a datasheet - there is none. See docs/HARDWARE.md.
 *
 * The register values below are the ones observed while the stock
 * firmware was playing, written in the same order and spelled out with
 * the TIM1 field names of the Aisinochip SDK (docs/SOC_IDENTIFICATION.md).
 *
 * Decoded, it is plain PWM: PWM mode 1 on channel 1, ARR = 255 so the
 * duty cycle in CCR1 takes 256 steps, and a DMA request on every CC1
 * event, i.e. one 8-bit sample per timer period.
 *
 * The sample rate, which is also the PWM carrier frequency, follows from
 * that: f_TIM / ((PSC + 1) * (ARR + 1)). PSC is computed from the clock
 * the way the stock firmware does it, aiming at 44.1 kHz, so the carrier
 * stays above hearing at any core clock. audio_rate() reports the
 * resulting rate to the mixer.
 */

#include "audio.h"
#include "clock.h"
#include "soc.h"

/*
 * TIM1 register values as observed in the running stock firmware.
 * Written in the same order the stock start sequence uses.
 */
#define TIM1_CR1_VAL    (TIM_CR1_ARPE | TIM_CR1_CEN)            /* 0x81  */
#define TIM1_DIER_VAL   TIM_DIER_CC1DE                          /* 0x200 */
#define TIM1_SR_VAL     (TIM_SR_CC1IF | TIM_SR_UIF)             /* 0x03  */
#define TIM1_EGR_VAL    0x00000000u
#define TIM1_CCMR1_VAL  (TIM_CCMR1_OC1M_PWM1 | TIM_CCMR1_OC1PE) /* 0x68  */
#define TIM1_ARR_VAL    255u       /* period of 256: 8-bit duty cycle   */

#define AUDIO_TARGET_HZ 44100u

/* Refill check rate. A buffer lasts ~16 ms, so 2 ms leaves ample margin. */
#define AUDIO_TICK_HZ   500u

/*
 * Audio DMA channel. The control word's low half is the remaining transfer
 * count (counts down while playing).
 */
#define DMA_CTRL_HI     0x85400000u
#define DMA_CFG_VAL     0x000A0083u
#define DMA_DONE_AUDIO  (1u << DMA_CH_AUDIO)

/* The buffers hold 16-bit words even though the useful range is 8 bits
 * (0..ARR); that is how the stock firmware feeds CCR1. */
static uint16_t buf[2][AUDIO_BUF_SAMPLES];

/*
 * The 8-word self-looping descriptor the stock firmware streams with:
 *
 *   +0x00 SRC   ping-pong base (firmware patches this)
 *   +0x04 DST   0x40012C34 (TIM1_CCR1)
 *   +0x08 NEXT  the descriptor itself
 *   +0x0C CTRL  0x854002E1
 *   +0x10       0x40012C00 (TIM1_BASE)
 *   +0x14       0x10       (same as TIM1_PSC)
 *   +0x18       0xFF       (same as TIM1_ARR)
 *   +0x1C       1
 *
 * A shorter descriptor stalls the channel.
 */
typedef struct {
    volatile uint32_t src;
    volatile uint32_t dst;
    volatile uint32_t next;
    volatile uint32_t ctrl;
    volatile uint32_t tim_base;
    volatile uint32_t w14;
    volatile uint32_t w18;
    volatile uint32_t w1c;
} dma_desc_t;

static dma_desc_t desc __attribute__((aligned(32)));
static unsigned playing;          /* index of the buffer the DMA is on */
static uint32_t blocks;
static int      running;
static uint32_t tim1_psc;
static uint32_t rate_hz;

static void audio_service(void);

uint32_t audio_blocks(void) { return blocks; }
uint32_t audio_rate(void)   { return rate_hz; }

/*
 * TIM1 clock, as the stock firmware derives it: the core clock divided by
 * the APB prescaler in RCC+0x20 bits 13:11 (4 = /2 ... 7 = /16), and
 * doubled whenever that prescaler divides.
 */
static uint32_t tim1_clock_hz(void)
{
    const uint32_t ppre = (REG32(RCC_BASE + 0x20) >> 11) & 7u;

    if (ppre < 4u)
        return g_cpu_hz;
    return (g_cpu_hz >> (ppre - 3u)) << 1;
}

/* PSC for AUDIO_TARGET_HZ, rounded to nearest: 16 at 194 MHz, which is
 * the value the stock firmware runs with. */
static uint32_t tim1_psc_for(uint32_t tim_hz)
{
    const uint32_t period = AUDIO_TARGET_HZ * (TIM1_ARR_VAL + 1u);
    const uint32_t div = (tim_hz + period / 2u) / period;

    return div ? div - 1u : 0u;
}

static void desc_set_src(unsigned idx)
{
    desc.src = (uint32_t)(uintptr_t)buf[idx];
}

/* As the stock audio init: channel SRC = buffer A, descriptor SRC =
 * buffer B, NEXT = &descriptor (self). */
static void dma_start(unsigned play)
{
    const unsigned nxt = play ^ 1u;

    desc.src      = (uint32_t)(uintptr_t)buf[nxt];
    desc.dst      = (uint32_t)(uintptr_t)&TIM1_CCR1;
    desc.next     = (uint32_t)(uintptr_t)&desc;
    desc.ctrl     = DMA_CTRL_HI | AUDIO_BUF_SAMPLES;
    desc.tim_base = TIM1_BASE;
    desc.w14      = tim1_psc;
    desc.w18      = TIM1_ARR_VAL;
    desc.w1c      = 1;

    DMA_CH_CFG(DMA_CH_AUDIO)  = DMA_CFG_VAL & ~1u;
    DMA_CH_SRC(DMA_CH_AUDIO)  = (uint32_t)(uintptr_t)buf[play];
    DMA_CH_DST(DMA_CH_AUDIO)  = desc.dst;
    DMA_CH_NEXT(DMA_CH_AUDIO) = desc.next;
    DMA_CH_CTRL(DMA_CH_AUDIO) = desc.ctrl;
    DMA_CH_CFG(DMA_CH_AUDIO)  = DMA_CFG_VAL | 1u;
}

void audio_init(void)
{
    if (running) return;

    const uint32_t tim_hz = tim1_clock_hz();
    tim1_psc = tim1_psc_for(tim_hz);
    rate_hz  = tim_hz / ((tim1_psc + 1u) * (TIM1_ARR_VAL + 1u));

    RCC_EN0 |= RCC_EN0_DMA1;

    /* TIM1 clock off, then on, as the stock audio init does. */
    RCC_EN2 &= ~RCC_EN2_TIM1;
    RCC_EN2 |=  RCC_EN2_TIM1;

    for (unsigned i = 0; i < 2; i++) {
        for (unsigned s = 0; s < AUDIO_BUF_SAMPLES; s++) {
            buf[i][s] = 128;                     /* silence */
        }
    }

    TIM1_CCER  = 0;                              /* off while configuring */
    TIM1_DIER  = TIM1_DIER_VAL;
    /* SR holds status flags; the stock value is replayed as observed. */
    TIM1_SR    = TIM1_SR_VAL;
    TIM1_EGR   = TIM1_EGR_VAL;
    TIM1_CCMR1 = TIM1_CCMR1_VAL;
    /* CNT (+0x24) is the timer counter, live while playing (Forest Kid:
     * it kept changing with the CPU halted). Do not treat 0x89 as config. */
    TIM1_PSC   = tim1_psc;
    TIM1_ARR   = TIM1_ARR_VAL;
    TIM1_CCR1  = 0;

    /* Route TIM1 channel 1 to the speaker: PA15 as AF2, pull-down,
     * drive 3, as the stock firmware does. */
    gpio_config(TIM1_CH1_PORT, TIM1_CH1_PIN, GPIO_MODE_AF, GPIO_PULL_DOWN,
                3u, TIM1_CH1_AF);

    /* Master enable of the DMA controller. Easy to miss: without it the
     * channel registers accept writes and read back correctly, the
     * channel reports enabled, and absolutely nothing moves. It only
     * turned up by diffing the whole block against the stock firmware. */
    DMA_GLOBAL_EN = 1;
    DMA_GLOBAL_1C = 1;          /* also 1 in the stock firmware */

    playing = 0;
    dma_start(0);

    TIM1_BDTR |= TIM_BDTR_MOE;
    TIM1_CR1   = TIM1_CR1_VAL;
    TIM1_CCER  = TIM_CCER_CC1E;

    running = 1;

    /* Lowest priority, so any interrupt added later preempts the mixer. */
    SCB_SHPR3 |= 0xFFu << 24;
    SYST_RVR = g_cpu_hz / AUDIO_TICK_HZ - 1u;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

#ifdef PROFILE
/* Cycles spent refilling the audio buffers, read over SWD. */
volatile uint32_t prof_audio_cyc;
#endif

void SysTick_Handler(void)
{
#ifdef PROFILE
    const uint32_t t0 = DWT_CYCCNT;
    audio_service();
    prof_audio_cyc += DWT_CYCCNT - t0;
#else
    audio_service();
#endif
}

/*
 * Runs in the SysTick interrupt, every 2 ms. The refill does not depend
 * on how long the game takes to render a frame: a buffer lasts ~16 ms,
 * and a refill that comes too late makes the DMA replay the old one.
 *
 * The switch is detected from the DMA's source pointer, not from a status
 * flag. Once the channel reloads itself through DMA_CH_NEXT the "done"
 * bit in DMA_STATUS stays at zero while the channel streams. The read
 * pointer says which buffer is being played, which is all a double buffer
 * needs.
 */
static void audio_service(void)
{
    if (!running) return;

    const uint32_t remain = DMA_CH_CTRL(DMA_CH_AUDIO) & 0xFFFFu;
    const uint32_t cfg    = DMA_CH_CFG(DMA_CH_AUDIO);
    const uint32_t src    = DMA_CH_SRC(DMA_CH_AUDIO);
    const unsigned cur    = (src >= (uint32_t)(uintptr_t)buf[1]) ? 1u : 0u;

    /* Hardware reloads from the self-loop descriptor. Patch SRC to the
     * idle buffer once DMA has moved onto the other one, as the stock
     * firmware does, while NEXT stays a self-pointer. */
    if ((cfg & 1u) && remain != 0) {
        if (cur == playing) return;
        playing = cur;
        blocks++;
        const unsigned idle = cur ^ 1u;
        audio_fill(buf[idle], AUDIO_BUF_SAMPLES);
        desc_set_src(idle);
        DMA_IRQ_CLR = DMA_DONE_AUDIO;
        return;
    }

    /* Transfer count hit zero and was not reloaded. Restart. */
    const unsigned nxt = playing ^ 1u;
    playing = nxt;
    blocks++;
    DMA_IRQ_CLR = DMA_DONE_AUDIO;
    audio_fill(buf[nxt ^ 1u], AUDIO_BUF_SAMPLES);
    dma_start(nxt);
}
