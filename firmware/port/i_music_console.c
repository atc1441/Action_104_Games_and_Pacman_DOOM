/*
 * i_music_console.c - DOOM's MUS music on the software OPL2
 *
 * The sequencer reads a D_* lump straight out of flash, 140 ticks a
 * second, and plays it on opl2.c with the GENMIDI instrument bank, the
 * way DOOM's DMX library drives a real OPL2: nine voices, instruments of
 * one or two voices, channel 15 for percussion, and the voice data
 * written to the chip's registers. Chocolate Doom's i_oplmusic.c, which
 * reproduces DMX, is the reference for those conventions.
 *
 * Everything below I_MusicRender runs in the audio interrupt. The game
 * only posts requests (start, stop, pause, volume) that the interrupt
 * picks up at its next block.
 */

#include <stdint.h>
#include <string.h>

#include "doomtype.h"
#include "sounds.h"
#include "i_sound.h"
#include "w_wad.h"

#include "opl2.h"
#include "i_music_console.h"

#define NUM_VOICES   OPL_CHANNELS
#define PERC_CHAN    15
#define MUS_TICK_HZ  140u

/* GENMIDI: "#OPL_II#", then 175 instruments of 36 bytes. */
#define GM_HEADER    8
#define GM_INSTR     36
#define GM_COUNT     175
#define GM_FIXED     0x01              /* plays fixed_note, not the key */
#define GM_DOUBLE    0x04              /* two voices                    */

/*
 * Lump names of the music enum, D_ dropped: the enum's own names upper
 * cased, which is how DOOM names them.
 */
static const char *const mus_names[NUMMUSIC] = {
    0,
    "E1M1", "E1M2", "E1M3", "E1M4", "E1M5", "E1M6", "E1M7", "E1M8", "E1M9",
    "E2M1", "E2M2", "E2M3", "E2M4", "E2M5", "E2M6", "E2M7", "E2M8", "E2M9",
    "E3M1", "E3M2", "E3M3", "E3M4", "E3M5", "E3M6", "E3M7", "E3M8", "E3M9",
    "INTER", "INTRO", "BUNNY", "VICTOR", "INTROA",
    "RUNNIN", "STALKS", "COUNTD", "BETWEE", "DOOM", "THE_DA", "SHAWN",
    "DDTBLU", "IN_CIT", "DEAD", "STLKS2", "THEDA2", "DOOM2", "DDTBL2",
    "RUNNI2", "DEAD2", "STLKS3", "ROMERO", "SHAWN2", "MESSAG", "COUNT2",
    "DDTBL3", "AMPIE", "THEDA3", "ADRIAN", "MESSG2", "ROMER2", "TENSE",
    "SHAWN3", "OPENIN", "EVIL", "ULTIMA", "READ_M", "DM2TTL", "DM2INT",
};

/*
 * pitch_q6: fnum << block, times 64, for MIDI notes 0-11 in steps of a
 * 32nd of a semitone; each octave up doubles it. voice_freq uses it one
 * octave up: most GENMIDI voices have frequency multiple 0, which halves
 * the chip's frequency, and measured that way the notes come out at their
 * MIDI pitch.
 * vol_att: OPL total-level steps (0.75 dB) for a MUS volume 0-127, so
 * that the amplitude follows the volume.
 */
static const uint16_t pitch_q6[384] = {
    11036, 11056, 11076, 11096, 11116, 11136, 11156, 11176, 11197, 11217, 11237, 11257,
    11278, 11298, 11319, 11339, 11359, 11380, 11401, 11421, 11442, 11462, 11483, 11504,
    11525, 11545, 11566, 11587, 11608, 11629, 11650, 11671, 11692, 11713, 11735, 11756,
    11777, 11798, 11820, 11841, 11862, 11884, 11905, 11927, 11948, 11970, 11992, 12013,
    12035, 12057, 12078, 12100, 12122, 12144, 12166, 12188, 12210, 12232, 12254, 12276,
    12298, 12321, 12343, 12365, 12388, 12410, 12432, 12455, 12477, 12500, 12522, 12545,
    12568, 12590, 12613, 12636, 12659, 12682, 12705, 12728, 12751, 12774, 12797, 12820,
    12843, 12866, 12889, 12913, 12936, 12959, 12983, 13006, 13030, 13053, 13077, 13100,
    13124, 13148, 13172, 13195, 13219, 13243, 13267, 13291, 13315, 13339, 13363, 13387,
    13412, 13436, 13460, 13484, 13509, 13533, 13558, 13582, 13607, 13631, 13656, 13680,
    13705, 13730, 13755, 13780, 13805, 13829, 13854, 13879, 13905, 13930, 13955, 13980,
    14005, 14031, 14056, 14081, 14107, 14132, 14158, 14183, 14209, 14235, 14260, 14286,
    14312, 14338, 14364, 14390, 14416, 14442, 14468, 14494, 14520, 14546, 14573, 14599,
    14625, 14652, 14678, 14705, 14731, 14758, 14785, 14811, 14838, 14865, 14892, 14919,
    14946, 14973, 15000, 15027, 15054, 15081, 15108, 15136, 15163, 15190, 15218, 15245,
    15273, 15301, 15328, 15356, 15384, 15411, 15439, 15467, 15495, 15523, 15551, 15579,
    15607, 15636, 15664, 15692, 15720, 15749, 15777, 15806, 15834, 15863, 15892, 15920,
    15949, 15978, 16007, 16036, 16065, 16094, 16123, 16152, 16181, 16210, 16240, 16269,
    16298, 16328, 16357, 16387, 16416, 16446, 16476, 16506, 16535, 16565, 16595, 16625,
    16655, 16685, 16715, 16746, 16776, 16806, 16837, 16867, 16897, 16928, 16959, 16989,
    17020, 17051, 17081, 17112, 17143, 17174, 17205, 17236, 17267, 17299, 17330, 17361,
    17393, 17424, 17456, 17487, 17519, 17550, 17582, 17614, 17646, 17677, 17709, 17741,
    17773, 17806, 17838, 17870, 17902, 17935, 17967, 17999, 18032, 18065, 18097, 18130,
    18163, 18195, 18228, 18261, 18294, 18327, 18360, 18394, 18427, 18460, 18493, 18527,
    18560, 18594, 18627, 18661, 18695, 18729, 18762, 18796, 18830, 18864, 18898, 18933,
    18967, 19001, 19035, 19070, 19104, 19139, 19173, 19208, 19243, 19277, 19312, 19347,
    19382, 19417, 19452, 19487, 19523, 19558, 19593, 19629, 19664, 19700, 19735, 19771,
    19807, 19842, 19878, 19914, 19950, 19986, 20022, 20058, 20095, 20131, 20167, 20204,
    20240, 20277, 20313, 20350, 20387, 20424, 20461, 20498, 20535, 20572, 20609, 20646,
    20683, 20721, 20758, 20796, 20833, 20871, 20909, 20946, 20984, 21022, 21060, 21098,
    21136, 21175, 21213, 21251, 21289, 21328, 21366, 21405, 21444, 21483, 21521, 21560,
    21599, 21638, 21677, 21716, 21756, 21795, 21834, 21874, 21913, 21953, 21993, 22032,
};

static const uint8_t vol_att[128] = {
    63, 56, 48, 43, 40, 37, 35, 34, 32, 31, 29, 28, 27, 26, 26, 25,
    24, 23, 23, 22, 21, 21, 20, 20, 19, 19, 18, 18, 18, 17, 17, 16,
    16, 16, 15, 15, 15, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12, 12,
    11, 11, 11, 11, 10, 10, 10, 10, 9, 9, 9, 9, 9, 8, 8, 8,
    8, 8, 8, 7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 5,
    5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
};
typedef struct {
    const uint8_t *instr;              /* GENMIDI entry                  */
    uint8_t        iv;                 /* which of its voices, 0 or 1    */
    int8_t         chan;               /* MUS channel, -1 when free      */
    uint8_t        key;                /* MUS note that started it       */
    uint8_t        vol;                /* its note volume                */
    uint32_t       age;
} voice_t;

typedef struct {
    uint8_t program, volume, note_vol;
    int8_t  bend;                      /* 32nds of a semitone, +-64      */
} chan_t;

static const uint8_t *genmidi;
static voice_t voices[NUM_VOICES];
static chan_t  chans[16];
static uint32_t voice_clock;

static const uint8_t *score, *score_end, *pos;
static uint32_t wait_ticks;
static uint32_t tick_acc, tick_step;   /* 16.16 ticks per sample        */
static int looping, playing;

/* Requests from the game, taken in I_MusicRender. */
static volatile const uint8_t *req_lump;
static volatile uint32_t req_len;
static volatile int req_play, req_stop, req_loop;
static volatile int paused;
static volatile int music_volume = 8;  /* 0..15 as the menu sets it     */

static const uint8_t chan_op[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };

/* ------------------------------------------------------------------ *
 * Voices
 * ------------------------------------------------------------------ */
/* Register base for voice n: its operator or channel offset, in its set. */
static unsigned op_reg(unsigned n)   { return ((n / 9u) << 8) | chan_op[n % 9u]; }
static unsigned chan_reg(unsigned n) { return ((n / 9u) << 8) | (n % 9u); }

static const uint8_t *voice_data(const voice_t *v)
{
    return v->instr + 4 + 16u * v->iv;
}

static void write_op(unsigned op, const uint8_t *d, unsigned level)
{
    opl_write(0x20 + op, d[0]);
    opl_write(0x60 + op, d[1]);
    opl_write(0x80 + op, d[2]);
    opl_write(0xE0 + op, d[3]);
    opl_write(0x40 + op, (d[4] & 0xC0u) | level);
}

static void voice_volume(unsigned n)
{
    const voice_t *v = &voices[n];
    const uint8_t *d = voice_data(v);
    const unsigned cv = (unsigned)chans[v->chan].volume * (unsigned)music_volume / 15u;

    unsigned car = (unsigned)vol_att[v->vol] + vol_att[cv];
    if (car > 63u) car = 63u;
    opl_write(0x40 + op_reg(n) + 3, (d[11] & 0xC0u) | car);

    /* Additive voices: the modulator is heard too, so it follows. */
    if ((d[6] & 1u) && (d[5] & 0x3Fu) != 0x3Fu) {
        unsigned mod = d[5] & 0x3Fu;
        if (mod < car) mod = car;
        opl_write(0x40 + op_reg(n), (d[4] & 0xC0u) | mod);
    }
}

static void voice_freq(unsigned n, int key_on)
{
    const voice_t *v = &voices[n];
    const uint8_t *d = voice_data(v);
    const unsigned flags = v->instr[0] | (v->instr[1] << 8);

    int note = (flags & GM_FIXED) ? v->instr[3] : v->key;
    if (!(flags & GM_FIXED))
        note += (int16_t)(d[14] | (d[15] << 8));
    while (note < 0)  note += 12;
    while (note > 95) note -= 12;

    int idx = 32 * note + chans[v->chan].bend;
    if (v->iv) idx += v->instr[2] / 2 - 64;  /* second voice detune */
    if (idx < 0) idx = 0;

    uint32_t x = (uint32_t)pitch_q6[idx % 384] << (idx / 384 + 1);
    unsigned block = 0;
    while (x >= (1024u << 6) && block < 7) { x >>= 1; block++; }
    unsigned fnum = x >> 6;
    if (fnum > 1023) fnum = 1023;

    opl_write(0xA0 + chan_reg(n), fnum & 0xFFu);
    opl_write(0xB0 + chan_reg(n), (key_on ? 0x20u : 0) | (block << 2) | (fnum >> 8));
}

static void voice_off(unsigned n)
{
    voice_freq(n, 0);
    voices[n].chan = -1;
}

static int voice_free(void)
{
    for (unsigned n = 0; n < NUM_VOICES; n++)
        if (voices[n].chan < 0) return (int)n;
    return -1;
}

/*
 * MUS channel as the MIDI channel DMX gives it: percussion is 9 and the
 * channels from 9 up move one along. Higher means less important.
 */
static unsigned chan_rank(int ch)
{
    if (ch == PERC_CHAN) return 9;
    return ch >= 9 ? (unsigned)ch + 1u : (unsigned)ch;
}

/*
 * A free voice, or else the one that matters least: a second voice of a
 * double-voice instrument first, then the highest ranked channel, and
 * among equals the newest note.
 */
static unsigned voice_alloc(void)
{
    const int f = voice_free();
    if (f >= 0) return (unsigned)f;

    unsigned best = 0;
    for (unsigned n = 1; n < NUM_VOICES; n++) {
        const voice_t *a = &voices[n], *b = &voices[best];
        if (a->iv != b->iv) {
            if (a->iv) best = n;
            continue;
        }
        const unsigned ra = chan_rank(a->chan), rb = chan_rank(b->chan);
        if (ra > rb || (ra == rb && a->age > b->age)) best = n;
    }
    voice_off(best);
    return best;
}

static void voice_on(int ch, unsigned key, unsigned vol,
                     const uint8_t *instr, unsigned iv)
{
    const unsigned n = voice_alloc();
    voice_t *v = &voices[n];

    v->instr = instr;
    v->iv    = (uint8_t)iv;
    v->chan  = (int8_t)ch;
    v->key   = (uint8_t)key;
    v->vol   = (uint8_t)vol;
    v->age   = ++voice_clock;

    const uint8_t *d = voice_data(v);
    const int modulating = !(d[6] & 1u);
    write_op(op_reg(n) + 3, d + 7, 0x3F);
    write_op(op_reg(n), d, modulating ? (d[5] & 0x3Fu) : 0x3Fu);
    opl_write(0xC0 + chan_reg(n), d[6]);

    voice_volume(n);
    voice_freq(n, 1);
}

/* ------------------------------------------------------------------ *
 * MUS events
 * ------------------------------------------------------------------ */
static void note_on(int ch, unsigned key, unsigned vol)
{
    unsigned ins;
    if (ch == PERC_CHAN) {
        if (key < 35 || key > 81) return;
        ins = 128 + key - 35;
    } else {
        ins = chans[ch].program;
    }
    if (ins >= GM_COUNT) return;

    const uint8_t *instr = genmidi + GM_HEADER + GM_INSTR * ins;
    voice_on(ch, key, vol, instr, 0);
    /* The second voice only adds body; it never takes another note's. */
    if (((instr[0] | (instr[1] << 8)) & GM_DOUBLE) && voice_free() >= 0)
        voice_on(ch, key, vol, instr, 1);
}

static void note_off(int ch, unsigned key)
{
    for (unsigned n = 0; n < NUM_VOICES; n++)
        if (voices[n].chan == ch && voices[n].key == key)
            voice_off(n);
}

static void chan_all_off(int ch)
{
    for (unsigned n = 0; n < NUM_VOICES; n++)
        if (voices[n].chan >= 0 && (ch < 0 || voices[n].chan == ch))
            voice_off(n);
}

static void chan_update(int ch, int freq)
{
    for (unsigned n = 0; n < NUM_VOICES; n++) {
        if (voices[n].chan != ch) continue;
        if (freq) voice_freq(n, 1);
        else      voice_volume(n);
    }
}

static void chans_reset(void)
{
    for (unsigned c = 0; c < 16; c++)
        chans[c] = (chan_t){ .program = 0, .volume = 127, .note_vol = 127,
                             .bend = 0 };
}

static int read_byte(unsigned *b)
{
    if (pos >= score_end) return 0;
    *b = *pos++;
    return 1;
}

static void song_stop(void)
{
    chan_all_off(-1);
    playing = 0;
}

static void song_rewind(void)
{
    chan_all_off(-1);
    chans_reset();
    pos = score;
    wait_ticks = 0;
}

/* One 140 Hz tick: run every event that is due. */
static void mus_tick(void)
{
    if (wait_ticks) { wait_ticks--; return; }

    for (;;) {
        unsigned ev, a, b;
        if (!read_byte(&ev)) { song_stop(); return; }
        const int ch = (int)(ev & 15u);

        switch ((ev >> 4) & 7u) {
        case 0:                                   /* release note */
            if (!read_byte(&a)) { song_stop(); return; }
            note_off(ch, a & 0x7Fu);
            break;
        case 1:                                   /* play note */
            if (!read_byte(&a)) { song_stop(); return; }
            if (a & 0x80u) {
                if (!read_byte(&b)) { song_stop(); return; }
                chans[ch].note_vol = (uint8_t)(b & 0x7Fu);
            }
            note_on(ch, a & 0x7Fu, chans[ch].note_vol);
            break;
        case 2:                                   /* pitch wheel */
            if (!read_byte(&a)) { song_stop(); return; }
            chans[ch].bend = (int8_t)(((int)a - 128) / 2);
            chan_update(ch, 1);
            break;
        case 3:                                   /* system event */
            if (!read_byte(&a)) { song_stop(); return; }
            if (a == 10 || a == 11) chan_all_off(ch);
            if (a == 14) {
                chans[ch].volume = 127;
                chans[ch].bend = 0;
                chan_update(ch, 1);
                chan_update(ch, 0);
            }
            break;
        case 4:                                   /* controller */
            if (!read_byte(&a) || !read_byte(&b)) { song_stop(); return; }
            if (a == 0) chans[ch].program = (uint8_t)(b & 0x7Fu);
            if (a == 3) {
                chans[ch].volume = (uint8_t)(b & 0x7Fu);
                chan_update(ch, 0);
            }
            break;
        case 5:                                   /* end of measure */
            break;
        case 6:                                   /* score end */
            if (looping) song_rewind();
            else         song_stop();
            return;
        default:
            song_stop();
            return;
        }

        if (ev & 0x80u) {                         /* a delay follows */
            uint32_t d = 0;
            do {
                if (!read_byte(&a)) { song_stop(); return; }
                d = (d << 7) | (a & 0x7Fu);
            } while (a & 0x80u);
            if (d) { wait_ticks = d - 1u; return; }
        }
    }
}

/* ------------------------------------------------------------------ *
 * Interface
 * ------------------------------------------------------------------ */
void I_MusicInit(uint32_t rate)
{
    opl_init(rate);
    opl_write(0x01, 0x20);                        /* waveform select on */
    tick_step = (uint32_t)(((uint64_t)MUS_TICK_HZ << 16) / rate);

    for (unsigned n = 0; n < NUM_VOICES; n++) voices[n].chan = -1;
    chans_reset();

    const int lump = W_CheckNumForName("GENMIDI");
    if (lump >= 0 && W_LumpLength(lump) >= GM_HEADER + GM_INSTR * GM_COUNT)
        genmidi = (const uint8_t *)W_CacheLumpNum(lump);
}

void I_MusicRender(int32_t *out, unsigned n)
{
    if (req_stop) {
        req_stop = 0;
        song_stop();
    }
    if (req_play) {
        const uint8_t *p = (const uint8_t *)req_lump;
        const uint32_t len = req_len;
        req_play = 0;
        const unsigned start = p[6] | (p[7] << 8);
        const unsigned slen  = p[4] | (p[5] << 8);
        if (start < len) {
            score = p + start;
            score_end = p + (start + slen <= len ? start + slen : len);
            looping = req_loop;
            song_rewind();
            tick_acc = 0;
            playing = 1;
        }
    }

    if (!playing || paused || !music_volume) return;

    while (n) {
        /* Samples up to the next tick, at most what is left. */
        unsigned run = (uint32_t)((0x10000u - tick_acc + tick_step - 1u)
                                  / tick_step);
        if (!run) run = 1;
        if (run > n) run = n;

        opl_render(out, run);
        out += run;
        n -= run;

        tick_acc += tick_step * run;
        if (tick_acc >= 0x10000u) {
            tick_acc -= 0x10000u;
            mus_tick();
            if (!playing) return;
        }
    }
}

void I_PlaySong(int handle, int looping_)
{
    if (!genmidi || handle <= 0 || handle >= NUMMUSIC || !mus_names[handle])
        return;

    char name[9] = "D_";
    strncpy(name + 2, mus_names[handle], 6);
    const int lump = W_CheckNumForName(name);
    if (lump < 0) return;

    const uint8_t *p = (const uint8_t *)W_CacheLumpNum(lump);
    const int32_t len = W_LumpLength(lump);
    if (len < 16 || memcmp(p, "MUS\x1a", 4) != 0) return;

    req_lump = p;
    req_len  = (uint32_t)len;
    req_loop = looping_;
    req_play = 1;
}

void I_StopSong(int handle)       { (void)handle; req_stop = 1; }
void I_PauseSong(int handle)      { (void)handle; paused = 1; }
void I_ResumeSong(int handle)     { (void)handle; paused = 0; }
void I_SetMusicVolume(int volume) { music_volume = volume; }
