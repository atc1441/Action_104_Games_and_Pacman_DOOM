/*
 * i_sound_console.c - software mixer and DOOM's sound hooks.
 *
 * This replaces GBADoom's i_audio.c, which is entirely inside #ifdef GBA
 * and calls Maxmod - a GBA library with nothing behind it here. The
 * Makefile filters that file out.
 *
 * The mixer is the classic DOOM one: eight channels, nearest-neighbour
 * resampling, summed into an unsigned buffer that sdk/audio.c streams by
 * DMA into TIM1, which plays it as 8-bit PWM.
 *
 * Sample data is NOT copied into RAM. DOOM's DS* lumps are read straight
 * out of the memory-mapped flash, the same trick the WAD itself uses -
 * which matters, because a handful of sounds would otherwise eat more RAM
 * than the framebuffer.
 */

#include <stdint.h>
#include <string.h>

#include "doomtype.h"
#include "sounds.h"
#include "i_sound.h"
#include "w_wad.h"
#include "lprintf.h"

#include "audio.h"
#include "input.h"

/*
 * Output rate, set from audio_rate() at init. TIM1 requests one sample per
 * PWM period and the driver picks the period for ~44.1 kHz, so the exact
 * rate depends on the core clock.
 */
static uint32_t out_rate;

#define NUM_CHANNELS 8

/*
 * 16.16 fixed point position, so a sound of any rate resamples to ours.
 *
 * audio_fill() runs in the SysTick interrupt and I_StartSound() in the
 * game, which the interrupt can preempt. A channel is only read while
 * `active` is set, and I_StartSound() clears it before rewriting the
 * other fields and sets it last, with compiler barriers in between.
 */
typedef struct {
    const uint8_t *data;      /* points into XIP flash */
    uint32_t       len;       /* samples */
    uint32_t       pos;       /* 16.16 */
    uint32_t       step;      /* 16.16 increment per output sample */
    int            vol;       /* 0..127 as DOOM hands it over */
    volatile int   active;
} chan_t;

#define COMPILER_BARRIER() __asm volatile ("" ::: "memory")

static chan_t chans[NUM_CHANNELS];
static int    sound_ready;

/*
 * A DOOM sound lump: 8-byte header, then unsigned 8-bit PCM.
 *   +0  uint16  format, always 3
 *   +2  uint16  sample rate
 *   +4  uint32  sample count
 */
static int lump_of(int sfxid)
{
    char name[9];
    const char *n = S_sfx[sfxid].name;

    if (!n) return -1;

    name[0] = 'D'; name[1] = 'S';
    unsigned i = 0;
    for (; i < 6 && n[i]; i++) {
        char c = n[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        name[2 + i] = c;
    }
    name[2 + i] = 0;

    return W_CheckNumForName(name);
}

int I_StartSound(int id, int channel, int vol, int sep)
{
    (void)sep;                      /* mono output, panning is dropped */

    if (!sound_ready) return -1;
    if (channel < 0 || channel >= NUM_CHANNELS) return -1;
    if (id <= 0 || id >= NUMSFX) return -1;

    const int lump = lump_of(id);
    if (lump < 0) return -1;

    const uint8_t *p = (const uint8_t *)W_CacheLumpNum(lump);
    if (!p) return -1;

    const unsigned rate = (unsigned)p[2] | ((unsigned)p[3] << 8);
    uint32_t count = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                     ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);

    const int32_t avail = W_LumpLength(lump) - 8;
    if (avail <= 0) return -1;
    if (count > (uint32_t)avail) count = (uint32_t)avail;
    if (!rate || !count) return -1;

    chan_t *c = &chans[channel];
    c->active = 0;                  /* stop before rewriting the fields */
    COMPILER_BARRIER();
    c->data = p + 8;
    c->len  = count;
    c->pos  = 0;
    c->step = (rate << 16) / out_rate;
    c->vol  = vol;
    COMPILER_BARRIER();
    c->active = 1;

    return channel;
}

#ifdef AUDIO_SELFTEST
/*
 * Temporary: work out what the output actually wants.
 *
 * Kept from bring-up, before the block was known to be TIM1. With
 * ARR = 255 the answer is format 1: CCR1 takes 0..255, and the other
 * formats put most values outside the PWM range.
 *
 * Faint noise and no tone means either the level is far too low or the
 * sample format is wrong, and those two look identical from the outside.
 * So instead of guessing, play a square wave through five candidate
 * formats in turn, each at its own pitch, and let a human say which one
 * came through:
 *
 *   1  220 Hz   8-bit unsigned, mid 128       (what the mixer does now)
 *   2  330 Hz   12-bit unsigned, mid 2048
 *   3  440 Hz   16-bit unsigned, mid 32768
 *   4  660 Hz   16-bit signed
 *   5  880 Hz   8-bit left-justified in 16
 *
 * Two seconds each, half a second of silence between, then it repeats.
 * Build with AUDIO_SELFTEST=1; not part of a normal image.
 */
static const uint16_t st_hz[5] = { 220, 330, 440, 660, 880 };

void audio_fill(uint16_t *dst, unsigned n)
{
    static uint32_t t;          /* samples since the cycle started */
    static unsigned variant;
    static unsigned phase;

    const uint32_t tone    = out_rate * 2u;
    const uint32_t silence = out_rate / 2u;

    for (unsigned i = 0; i < n; i++) {
        if (t >= tone + silence) { t = 0; variant = (variant + 1) % 5; }

        uint16_t v;
        if (t >= tone) {
            /* silence, in the format under test */
            switch (variant) {
                case 0:  v = 128;    break;
                case 1:  v = 2048;   break;
                case 2:  v = 32768;  break;
                case 3:  v = 0;      break;
                default: v = 0x8000; break;
            }
        } else {
            const uint32_t half = out_rate / (2u * st_hz[variant]);
            phase++;
            if (phase >= 2u * half) phase = 0;
            const int high = phase < half;

            switch (variant) {
                case 0:  v = high ? 255u   : 1u;      break;
                case 1:  v = high ? 4095u  : 1u;      break;
                case 2:  v = high ? 65535u : 1u;      break;
                case 3:  v = high ? 32767u : 0x8001u; break;
                default: v = high ? 0xFF00u : 0x0100u; break;
            }
        }
        dst[i] = v;
        t++;
    }
}
#else
/*
 * Called from sdk/audio.c when a DMA block has been consumed.
 *
 * Output is unsigned 0..255 with 128 as silence, matching what the stock
 * firmware feeds TIM1 (ARR = 255, so that is the full duty-cycle range). The accumulator is signed so that channels can
 * cancel; it is clamped rather than wrapped, because wrapping a mix turns
 * a loud moment into a burst of noise.
 */
void audio_fill(uint16_t *dst, unsigned n)
{
    /* Stock DAT_2000044c is 0..3 with divisors 5 / 2 / 1 (and 0 = mute).
     * Extra shifts of 2 / 1 / 0 are 1/4 / 1/2 / full. Mute zeros the mix. */
    const unsigned v = input_volume();
    const unsigned shift = (v == 0) ? 0 : (7u + 3u - v);

    for (unsigned i = 0; i < n; i++) {
        int32_t acc = 0;

        for (unsigned ch = 0; ch < NUM_CHANNELS; ch++) {
            chan_t *c = &chans[ch];
            if (!c->active) continue;

            const uint32_t idx = c->pos >> 16;
            if (idx >= c->len) { c->active = 0; continue; }

            /* 0..255 unsigned -> -128..127, scaled by volume */
            acc += ((int32_t)c->data[idx] - 128) * c->vol;
            c->pos += c->step;
        }

        /*
         * Volume arrives as 0..127, not 0..15 - measured on the device,
         * a menu blip came through at 120. Shifting by 4 instead of 7
         * overdrives every sample by a factor of eight and the clamp
         * below turns the waveform into a square, which sounds like
         * distortion rather than DOOM.
         */
        if (v == 0) acc = 0;
        else        acc >>= shift;
        acc += 128;
        if (acc < 0)   acc = 0;
        if (acc > 255) acc = 255;

        dst[i] = (uint16_t)acc;
    }
}

#endif /* AUDIO_SELFTEST */

void I_InitSound(void)
{
    for (unsigned i = 0; i < NUM_CHANNELS; i++) chans[i].active = 0;

    audio_init();
    out_rate = audio_rate();
    sound_ready = 1;

    lprintf(LO_INFO, "I_InitSound: TIM1 PWM at %lu Hz", (unsigned long)out_rate);
}

/*
 * Music. DOOM's MUS format needs an OPL synth to sound like anything, and
 * that is a poor trade on a core that is already busy rendering.
 * The hooks stay so the engine has something to call.
 */
void I_PlaySong(int handle, int looping)  { (void)handle; (void)looping; }
void I_PauseSong(int handle)              { (void)handle; }
void I_ResumeSong(int handle)             { (void)handle; }
void I_StopSong(int handle)               { (void)handle; }
void I_SetMusicVolume(int volume)         { (void)volume; }
