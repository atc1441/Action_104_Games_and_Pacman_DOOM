/*
 * audio.c - DAC output driven by DMA
 *
 * Rebuilt from the stock firmware, which was read live over SWD rather
 * than derived from a datasheet - there is none. See docs/HARDWARE.md.
 *
 * The register values below are replayed exactly as they were observed
 * while the stock firmware was playing. That is deliberate: what the
 * individual bits mean is not established, and inventing plausible
 * meanings for them would be guessing dressed up as understanding. Once
 * sound comes out, the individual fields can be probed one at a time.
 *
 * The one number this does NOT give us is the sample rate. Nothing in the
 * observed registers identifies it - +0x18 = 0x68 is the obvious
 * candidate for a divider, but that is a guess. audio_blocks() exists so
 * the rate can be measured from the host instead of assumed; see
 * docs/HARDWARE.md for how.
 */

#include "audio.h"
#include "soc.h"

/*
 * DAC register values as observed in the running stock firmware.
 * Written in the same order the stock start sequence uses.
 */
#define DAC_R00_VAL   0x00000081u
#define DAC_R0C_VAL   0x00000200u
#define DAC_R10_VAL   0x00000003u
#define DAC_R14_VAL   0x00000000u
#define DAC_R18_VAL   0x00000068u
#define DAC_R28_VAL   0x00000010u
#define DAC_R2C_VAL   0x000000FFu
#define DAC_R44_BIT   0x00008000u

/*
 * DMA channel 0. The control word's low half is the remaining transfer
 * count (counts down while playing). Reload value from the stock
 * descriptor while Forest Kid was playing: 0x854002E1.
 */
#define DMA_CTRL_HI   0x85400000u
#define DMA_CFG_VAL   0x000A0083u
#define DMA_DONE_CH0  (1u << 0)

/* The buffers hold 16-bit words even though the useful range is 8 bits;
 * that is how the stock firmware feeds this DAC. */
static uint16_t buf[2][AUDIO_BUF_SAMPLES];

/*
 * Live Forest Kid (stock, playing): DMA ch0 +0x08 stayed at 0x20044BA0,
 * an 8-word self-looping descriptor:
 *
 *   +0x00 SRC   ping-pong base (firmware patches this)
 *   +0x04 DST   0x40012C34
 *   +0x08 NEXT  0x20044BA0 (self)
 *   +0x0C CTRL  0x854002E1
 *   +0x10       0x40012C00
 *   +0x14       0x10
 *   +0x18       0xFF
 *   +0x1C       1
 *
 * Earlier we pointed +0x08 at a single next-SRC word; the controller
 * consumed it and stopped. A 4-word chain also stalled. The missing
 * words are what the hardware reloads.
 */
typedef struct {
    volatile uint32_t src;
    volatile uint32_t dst;
    volatile uint32_t next;
    volatile uint32_t ctrl;
    volatile uint32_t dac_base;
    volatile uint32_t w14;
    volatile uint32_t w18;
    volatile uint32_t w1c;
} dma_desc_t;

static dma_desc_t desc __attribute__((aligned(32)));
static unsigned playing;          /* index of the buffer the DMA is on */
static uint32_t blocks;
static int      running;

uint32_t audio_blocks(void) { return blocks; }

static void desc_set_src(unsigned idx)
{
    desc.src = (uint32_t)(uintptr_t)buf[idx];
}

/* Stock FUN_0803f080: channel SRC = buffer A, descriptor SRC = buffer B,
 * NEXT = &descriptor (self). The extra four words are what sat after the
 * 4-word SG fields in RAM while Forest Kid played; DMA reloads them. */
static void dma_start(unsigned play)
{
    const unsigned nxt = play ^ 1u;

    desc.src      = (uint32_t)(uintptr_t)buf[nxt];
    desc.dst      = (uint32_t)(uintptr_t)&AUDIO_DATA;
    desc.next     = (uint32_t)(uintptr_t)&desc;
    desc.ctrl     = DMA_CTRL_HI | AUDIO_BUF_SAMPLES;
    desc.dac_base = AUDIO_BASE;
    desc.w14      = DAC_R28_VAL;
    desc.w18      = DAC_R2C_VAL;
    desc.w1c      = 1;

    DMA_CH_CFG(0)  = DMA_CFG_VAL & ~1u;
    DMA_CH_SRC(0)  = (uint32_t)(uintptr_t)buf[play];
    DMA_CH_DST(0)  = desc.dst;
    DMA_CH_NEXT(0) = desc.next;
    DMA_CH_CTRL(0) = desc.ctrl;
    DMA_CH_CFG(0)  = DMA_CFG_VAL | 1u;
}

void audio_init(void)
{
    if (running) return;

    /*
     * Clocks.
     *
     * These bits were found by diffing our RCC state against the stock
     * firmware's while it was playing: EN0 bits 0-1 and EN2 bit 9 were
     * the only ones missing. Without them the DMA controller has no clock
     * and its registers read back as zero however often you write them -
     * which is exactly the symptom that led here.
     *
     * Which of the three is the DMA and which the DAC is not established;
     * setting what the stock firmware sets is the honest option.
     */
    RCC_EN0 |= 0x00000003u;
    RCC_EN2 |= 0x00000200u;

    /* Reset the audio block: RCC+0x38 bit 6 off, then on, as the stock
     * firmware does around its audio setup. */
    RCC_EN2 &= ~RCC_EN2_AUDIO;
    RCC_EN2 |=  RCC_EN2_AUDIO;

    for (unsigned i = 0; i < 2; i++) {
        for (unsigned s = 0; s < AUDIO_BUF_SAMPLES; s++) {
            buf[i][s] = 128;                     /* silence */
        }
    }

    AUDIO_EN = 0;                                /* off while configuring */
    REG32(AUDIO_BASE + 0x0C) = DAC_R0C_VAL;
    REG32(AUDIO_BASE + 0x10) = DAC_R10_VAL;
    REG32(AUDIO_BASE + 0x14) = DAC_R14_VAL;
    REG32(AUDIO_BASE + 0x18) = DAC_R18_VAL;
    /* +0x24 is a live hardware counter while playing (Forest Kid: it
     * kept changing with the CPU halted). Do not treat 0x89 as config. */
    REG32(AUDIO_BASE + 0x28) = DAC_R28_VAL;
    REG32(AUDIO_BASE + 0x2C) = DAC_R2C_VAL;
    AUDIO_DATA = 0;

    /*
     * Put PA6 and PA7 into analog mode - that is how the stock firmware
     * leaves them, and analog on a pin that is neither GPIO nor an
     * alternate function is what a DAC output looks like.
     *
     * This matters because lcd_gpio_init() drives PA6 back to input as a
     * side effect of its MODER mask, which disconnects the output. With
     * the pin left as an input the DAC converts into nothing: the DMA
     * runs, the block counter advances, and the speaker stays silent
     * apart from coupling noise - which is exactly what was heard.
     */
    {
        uint32_t m = GPIO_MODER(GPIOA_BASE);
        m |= (3u << (6 * 2)) | (3u << (7 * 2));      /* 11 = analog */
        GPIO_MODER(GPIOA_BASE) = m;

        /*
         * GPIOA +0x1C and +0x28, as the stock firmware has them.
         *
         * These are needed: without them the DMA channel arms correctly
         * and then sits there - source pointer and transfer count frozen,
         * because the DAC never raises a request. With them, sound comes
         * out. What they actually do is not established; +0x20/+0x24 are
         * the alternate-function selects, so these are something adjacent
         * to pin routing.
         *
         * OR rather than assignment on purpose. Overwriting them outright
         * took the debug port down with it - PA13/PA14 are SWD and sit on
         * this same port - and a console that makes noise but cannot be
         * debugged is a poor trade. Merging leaves the existing bits in
         * place.
         */
        REG32(GPIOA_BASE + 0x1C) |= 0x20000114u;
        REG32(GPIOA_BASE + 0x28) |= 0x00009F3Fu;
    }

    /* Master enable of the DMA controller. Easy to miss: without it the
     * channel registers accept writes and read back correctly, the
     * channel reports enabled, and absolutely nothing moves. It only
     * turned up by diffing the whole block against the stock firmware. */
    DMA_GLOBAL_EN = 1;
    DMA_GLOBAL_1C = 1;          /* also 1 in the stock firmware */

    playing = 0;
    dma_start(0);

    AUDIO_CR44 |= DAC_R44_BIT;
    AUDIO_CR0   = DAC_R00_VAL;
    AUDIO_EN    = 1;

    running = 1;
}

/*
 * Called from I_GetTime_e32(), which the engine hits in tight loops - see
 * the note there. Polling rather than an interrupt keeps this out of the
 * vector table entirely, which matters because the IRQ numbering of this
 * SoC is not established either.
 *
 * The switch is detected from the DMA's source pointer, not from a status
 * flag. Once the channel reloads itself through DMA_CH_NEXT the "done"
 * bit in DMA_STATUS never turns up - it stayed at zero through a six
 * second measurement while the channel was demonstrably streaming. The
 * read pointer, on the other hand, says plainly which buffer is being
 * played, and that is all the information a double buffer needs.
 *
 * Symptom when this is wrong: one buffer repeats forever, which comes out
 * of the speaker as a steady high whistle rather than silence.
 */
void audio_service(void)
{
    if (!running) return;

    const uint32_t remain = DMA_CH_CTRL(0) & 0xFFFFu;
    const uint32_t cfg    = DMA_CH_CFG(0);
    const uint32_t src    = DMA_CH_SRC(0);
    const unsigned cur    = (src >= (uint32_t)(uintptr_t)buf[1]) ? 1u : 0u;

    /* Hardware reloads from the self-loop descriptor. Patch SRC to the
     * idle buffer once DMA has moved onto the other one, matching stock
     * which rewrites 0x20044BA0 while NEXT stays a self-pointer. */
    if ((cfg & 1u) && remain != 0) {
        if (cur == playing) return;
        playing = cur;
        blocks++;
        const unsigned idle = cur ^ 1u;
        audio_fill(buf[idle], AUDIO_BUF_SAMPLES);
        desc_set_src(idle);
        DMA_IRQ_CLR = DMA_DONE_CH0;
        return;
    }

    /* Transfer count hit zero and was not reloaded. Restart. */
    const unsigned nxt = playing ^ 1u;
    playing = nxt;
    blocks++;
    DMA_IRQ_CLR = DMA_DONE_CH0;
    audio_fill(buf[nxt ^ 1u], AUDIO_BUF_SAMPLES);
    dma_start(nxt);
}
