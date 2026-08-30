/*
 * i_sound_console.c - software mixer and DOOM's sound hooks.
 *
 * This replaces GBADoom's i_audio.c, which is entirely inside #ifdef GBA
 * and calls Maxmod - a GBA library with nothing behind it here. The
 * Makefile filters that file out.
 *
 * The mixer is the classic DOOM one: eight channels, nearest-neighbour
 * resampling, summed into an unsigned buffer that sdk/audio.c streams to
 * the DAC by DMA.
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

const int snd_samplerate = 14016;

/*
 * Output rate, measured rather than assumed: audio_blocks() counts
 * completed DMA blocks, and 741 samples per block over 12 seconds gave
 * 14016 Hz (a 3-second run gave 14071, so it is stable). Forest Kid's
 * live descriptor reloads 737 samples; the DAC divider is unchanged, so
 * the rate stands until it is measured again.
 *
 * The DAC's rate divider was never identified in the register map, so
 * this number comes from the stopwatch, not the datasheet. If you change
 * the DAC configuration in sdk/audio.c, measure it again.
 */
#define OUT_RATE 14016u

#define NUM_CHANNELS 8

/* 16.16 fixed point position, so a sound of any rate resamples to ours */
typedef struct {
    const uint8_t *data;      /* points into XIP flash */
    uint32_t       len;       /* samples */
    uint32_t       pos;       /* 16.16 */
    uint32_t       step;      /* 16.16 increment per output sample */
    int            vol;       /* 0..127 as DOOM hands it over */
    int            active;
} chan_t;

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
    c->data = p + 8;
    c->len  = count;
    c->pos  = 0;
    c->step = (rate << 16) / OUT_RATE;
    c->vol  = vol;
    c->active = 1;

    return channel;
}

#ifdef AUDIO_SELFTEST
/*
 * Temporary: work out what the DAC actually wants.
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

    const uint32_t tone    = OUT_RATE * 2u;
    const uint32_t silence = OUT_RATE / 2u;

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
            const uint32_t half = OUT_RATE / (2u * st_hz[variant]);
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
 * Output is unsigned with 128 as silence, matching what the stock
 * firmware feeds this DAC. The accumulator is signed so that channels can
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
    sound_ready = 1;

    lprintf(LO_INFO, "I_InitSound: DAC ready");
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
