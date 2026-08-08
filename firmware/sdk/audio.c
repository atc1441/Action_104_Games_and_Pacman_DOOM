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
#define DAC_R24_VAL   0x00000089u
#define DAC_R28_VAL   0x00000010u
#define DAC_R2C_VAL   0x000000FFu
#define DAC_R44_BIT   0x00008000u

/*
 * DMA channel 0. The control word's low half is the transfer count - it
 * was seen counting down - so the upper half is the actual configuration
 * and the count goes in fresh on every restart.
 */
#define DMA_CTRL_HI   0x85400000u
#define DMA_CFG_VAL   0x000A0083u
#define DMA_DONE_CH0  (1u << 0)

/* The buffers hold 16-bit words even though the useful range is 8 bits;
 * that is how the stock firmware feeds this DAC. */
static uint16_t buf[2][AUDIO_BUF_SAMPLES];

/*
 * The channel's +0x08 points at a word holding the source address for the
 * next run. The controller consumes it - after one reload it read back as
 * zero and the channel stopped - so it has to be re-armed every time.
 *
 * A circular chain of two descriptors was tried instead, on the theory
 * that +0x08 walks a scatter-gather list whose fields mirror the channel
 * registers. The channel then transferred a couple of hundred samples and
 * stalled, so that layout is wrong. Re-arming from software costs one
 * store per buffer and is known to work.
 */
static volatile uint32_t dma_next;
static unsigned playing;          /* index of the buffer the DMA is on */
static uint32_t blocks;
static int      running;

uint32_t audio_blocks(void) { return blocks; }

static void dma_arm(unsigned play, unsigned nxt)
{
    dma_next = (uint32_t)(uintptr_t)buf[nxt];

    DMA_CH_CFG(0)  = DMA_CFG_VAL & ~1u;          /* stop the channel */
    DMA_CH_SRC(0)  = (uint32_t)(uintptr_t)buf[play];
    DMA_CH_DST(0)  = (uint32_t)(uintptr_t)&AUDIO_DATA;
    DMA_CH_NEXT(0) = (uint32_t)(uintptr_t)&dma_next;
    DMA_CH_CTRL(0) = DMA_CTRL_HI | AUDIO_BUF_SAMPLES;
    DMA_CH_CFG(0)  = DMA_CFG_VAL | 1u;           /* go */
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
    REG32(AUDIO_BASE + 0x24) = DAC_R24_VAL;
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
    dma_arm(0, 1);

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

    const uint32_t src = DMA_CH_SRC(0);
    const unsigned cur = (src >= (uint32_t)(uintptr_t)buf[1]) ? 1u : 0u;

    if (cur == playing) return;          /* still on the same buffer */

    playing = cur;
    blocks++;

    const unsigned idle = cur ^ 1u;

    /* Re-arm: the controller consumed the pointer when it reloaded. */
    dma_next = (uint32_t)(uintptr_t)buf[idle];
    DMA_CH_NEXT(0) = (uint32_t)(uintptr_t)&dma_next;

    /* Clear the completion flag if the controller does raise it - costs
     * nothing and keeps the block from latching something stale. */
    DMA_IRQ_CLR = DMA_DONE_CH0;

    audio_fill(buf[idle], AUDIO_BUF_SAMPLES);
}
