/*
 * opl2.c - a small software OPL2 (YM3812)
 *
 * Enough of the chip for DOOM's music: nine channels of two operators,
 * FM or additive, operator feedback, the four OPL2 waveforms, frequency
 * multiple, total level, key scale level, ADSR with key scale rate and
 * both envelope types, and tremolo and vibrato. Rhythm mode, CSM and the
 * timers are left out; DOOM's driver uses none of them.
 *
 * Everything is integer. Each operator is a phase accumulator into a
 * quarter-sine table, scaled by a gain looked up from its attenuation.
 * Envelopes and the two LFOs advance once per block of BLOCK samples,
 * not per sample as on the chip.
 *
 * The behaviour follows the YM3812 application manual's figures: decay
 * and release take 39.28 s to fall 96 dB at rate 1 and halve every rate
 * step, attack takes 2.83 s at rate 1, tremolo is 1 or 4.8 dB at 3.7 Hz,
 * vibrato 7 or 14 cents at 6.1 Hz. Register decoding, key scaling and the
 * feedback path follow Nuked OPL3. The output is linear rather than the
 * chip's log-sin and exponent tables, so it is close but not bit-exact.
 */

#include "opl2.h"

#define BLOCK      16u                 /* samples per envelope/LFO step */
#define CHIP_HZ    49716u              /* the real chip's sample rate   */
#define ENV_MAX    (511u << 16)        /* silent, Q16 attenuation steps */

enum { EG_OFF, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

typedef struct {
    uint32_t phase;                    /* one cycle = 2^32               */
    uint32_t inc;                      /* per sample, from fnum and MULT */
    uint32_t inc_now;                  /* inc with vibrato applied       */
    uint32_t env;                      /* attenuation, 0.1875 dB, Q16    */
    int32_t  out0, out1;               /* last two outputs, feedback     */
    uint16_t gain;                     /* this block's gain, Q12         */
    uint16_t ksl_att;                  /* key scale level, env units     */
    uint8_t  state;
    uint8_t  am, vib, egt, ksr, mult, ksl, tl;
    uint8_t  ar, dr, sl, rr, ws, rof;
} op_t;

typedef struct {
    op_t     op[2];                    /* modulator, carrier             */
    uint16_t fnum;
    uint8_t  block, key, fb, cnt;
} chan_t;

static chan_t   ch[OPL_CHANNELS];
static uint32_t out_rate;
static uint8_t  wse, dam, dvb;
static uint32_t dec_inc[64];           /* env units per block, Q16       */
static uint32_t atk_k[64];             /* fraction of env per block, Q16 */
static uint32_t lfo_am, lfo_vib;       /* LFO phases                     */
static uint32_t lfo_am_inc, lfo_vib_inc;
static unsigned blk_left;

static const uint16_t quarter_sine[256] = {
    13, 38, 63, 88, 113, 138, 163, 188, 213, 239, 264, 289,
    314, 339, 364, 389, 414, 439, 464, 489, 514, 539, 564, 588,
    613, 638, 663, 688, 712, 737, 762, 787, 811, 836, 860, 885,
    909, 934, 958, 983, 1007, 1032, 1056, 1080, 1104, 1128, 1153, 1177,
    1201, 1225, 1249, 1273, 1296, 1320, 1344, 1368, 1391, 1415, 1439, 1462,
    1485, 1509, 1532, 1555, 1579, 1602, 1625, 1648, 1671, 1694, 1717, 1739,
    1762, 1785, 1807, 1830, 1852, 1875, 1897, 1919, 1941, 1964, 1986, 2007,
    2029, 2051, 2073, 2094, 2116, 2137, 2159, 2180, 2201, 2223, 2244, 2265,
    2285, 2306, 2327, 2348, 2368, 2389, 2409, 2429, 2449, 2470, 2490, 2509,
    2529, 2549, 2569, 2588, 2608, 2627, 2646, 2665, 2684, 2703, 2722, 2741,
    2759, 2778, 2796, 2815, 2833, 2851, 2869, 2887, 2904, 2922, 2940, 2957,
    2974, 2992, 3009, 3026, 3043, 3059, 3076, 3093, 3109, 3125, 3141, 3157,
    3173, 3189, 3205, 3221, 3236, 3251, 3267, 3282, 3297, 3311, 3326, 3341,
    3355, 3370, 3384, 3398, 3412, 3426, 3439, 3453, 3466, 3480, 3493, 3506,
    3519, 3532, 3544, 3557, 3569, 3581, 3594, 3606, 3617, 3629, 3641, 3652,
    3663, 3675, 3686, 3696, 3707, 3718, 3728, 3739, 3749, 3759, 3769, 3778,
    3788, 3798, 3807, 3816, 3825, 3834, 3843, 3851, 3860, 3868, 3876, 3884,
    3892, 3900, 3908, 3915, 3922, 3929, 3936, 3943, 3950, 3957, 3963, 3969,
    3975, 3981, 3987, 3993, 3998, 4004, 4009, 4014, 4019, 4023, 4028, 4033,
    4037, 4041, 4045, 4049, 4053, 4056, 4059, 4063, 4066, 4069, 4071, 4074,
    4076, 4079, 4081, 4083, 4085, 4087, 4088, 4089, 4091, 4092, 4093, 4093,
    4094, 4095, 4095, 4095,
};

static const uint16_t att_gain[512] = {
    4096, 4009, 3923, 3839, 3757, 3677, 3598, 3522, 3446, 3373, 3301, 3230,
    3161, 3094, 3028, 2963, 2900, 2838, 2777, 2718, 2660, 2603, 2547, 2493,
    2440, 2388, 2337, 2287, 2238, 2190, 2143, 2098, 2053, 2009, 1966, 1924,
    1883, 1843, 1803, 1765, 1727, 1690, 1654, 1619, 1584, 1551, 1517, 1485,
    1453, 1422, 1392, 1362, 1333, 1305, 1277, 1249, 1223, 1197, 1171, 1146,
    1122, 1098, 1074, 1051, 1029, 1007, 985, 964, 944, 924, 904, 885,
    866, 847, 829, 811, 794, 777, 761, 744, 728, 713, 698, 683,
    668, 654, 640, 626, 613, 600, 587, 574, 562, 550, 538, 527,
    516, 505, 494, 483, 473, 463, 453, 443, 434, 425, 416, 407,
    398, 389, 381, 373, 365, 357, 350, 342, 335, 328, 321, 314,
    307, 301, 294, 288, 282, 276, 270, 264, 258, 253, 248, 242,
    237, 232, 227, 222, 217, 213, 208, 204, 199, 195, 191, 187,
    183, 179, 175, 171, 168, 164, 161, 157, 154, 151, 147, 144,
    141, 138, 135, 132, 130, 127, 124, 121, 119, 116, 114, 111,
    109, 107, 104, 102, 100, 98, 96, 94, 92, 90, 88, 86,
    84, 82, 81, 79, 77, 76, 74, 72, 71, 69, 68, 66,
    65, 64, 62, 61, 60, 58, 57, 56, 55, 53, 52, 51,
    50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 40,
    39, 38, 37, 36, 35, 35, 34, 33, 33, 32, 31, 30,
    30, 29, 29, 28, 27, 27, 26, 26, 25, 25, 24, 24,
    23, 23, 22, 22, 21, 21, 20, 20, 19, 19, 19, 18,
    18, 17, 17, 17, 16, 16, 16, 15, 15, 15, 14, 14,
    14, 13, 13, 13, 13, 12, 12, 12, 12, 11, 11, 11,
    11, 10, 10, 10, 10, 10, 9, 9, 9, 9, 9, 8,
    8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 6,
    6, 6, 6, 6, 6, 6, 6, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};
/* MULT as twice the factor, so that 0 can mean one half. */
static const uint8_t mult2[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};

static const uint8_t ksl_rom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};

/* KSL 0 / 1 / 2 / 3 is none / 1.5 / 3 / 6 dB per octave. */
static const uint8_t ksl_shift[4] = { 15, 1, 2, 0 };

/* Channel -> modulator operator offset; the carrier is 3 further on. */
static const uint8_t chan_op[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };

static void op_freq(chan_t *c, op_t *o)
{
    o->inc = (uint32_t)(((uint64_t)CHIP_HZ * ((uint32_t)c->fnum << c->block)
                         * mult2[o->mult] * 2048u) / out_rate);
    o->inc_now = o->inc;

    int k = (int)ksl_rom[c->fnum >> 6] * 4 - (8 - (int)c->block) * 32;
    o->ksl_att = (uint16_t)(k < 0 ? 0 : k);

    const unsigned ksv = (c->block << 1) | ((c->fnum >> 9) & 1u);
    o->rof = (uint8_t)(o->ksr ? ksv : ksv >> 2);
}

static unsigned eg_rate(const op_t *o, unsigned r)
{
    if (!r) return 0;
    r = r * 4u + o->rof;
    return r > 63u ? 63u : r;
}

/* One envelope step, then the gain for the samples up to the next. */
static void op_block(op_t *o, unsigned am_att, int32_t vib_q16)
{
    switch (o->state) {
    case EG_ATTACK: {
        const unsigned r = eg_rate(o, o->ar);
        if (r >= 60u) {
            o->env = 0;
        } else if (r >= 4u) {
            o->env -= (uint32_t)(((uint64_t)o->env * atk_k[r]) >> 16);
            if (o->env < (1u << 16)) o->env = 0;
        }
        if (!o->env) o->state = EG_DECAY;
        break;
    }
    case EG_DECAY: {
        const uint32_t sl = (o->sl == 15 ? 31u : o->sl) << (4 + 16);
        o->env += dec_inc[eg_rate(o, o->dr)];
        if (o->env >= sl) { o->env = sl; o->state = EG_SUSTAIN; }
        break;
    }
    case EG_SUSTAIN:
        if (o->egt) break;
        /* fall through: without EGT the level keeps falling at RR */
    case EG_RELEASE:
        o->env += dec_inc[eg_rate(o, o->rr)];
        if (o->env >= ENV_MAX) { o->env = ENV_MAX; o->state = EG_OFF; }
        break;
    default:
        break;
    }

    unsigned att = (o->env >> 16) + ((unsigned)o->tl << 2)
                 + (o->ksl_att >> ksl_shift[o->ksl]) + (o->am ? am_att : 0u);
    o->gain = att > 511u ? 0 : att_gain[att];

    o->inc_now = o->vib
        ? o->inc + (uint32_t)(((int64_t)o->inc * vib_q16) >> 16)
        : o->inc;
}

/* Signed sample of waveform ws at a 10-bit phase. */
static inline int32_t wave(unsigned ws, unsigned idx)
{
    idx &= 1023u;
    unsigned q = idx & 255u;
    if (idx & 256u) q = 255u - q;
    const int32_t v = quarter_sine[q];

    switch (ws) {
    case 0:  return (idx & 512u) ? -v : v;    /* sine           */
    case 1:  return (idx & 512u) ? 0 : v;     /* half sine      */
    case 2:  return v;                        /* absolute sine  */
    default: return (idx & 256u) ? 0 : v;     /* quarter pulses */
    }
}

void opl_init(uint32_t rate)
{
    out_rate = rate;
    for (unsigned r = 4; r < 64; r++) {
        const unsigned sh = r / 4u - 1u, frac = 4u + (r & 3u);
        /* 96 dB in 39.28 s at rate 4, i.e. 511 env units */
        dec_inc[r] = (uint32_t)((((uint64_t)511u << 16) * BLOCK * 1000u
                                 * frac << sh) / (39280ull * 4u * rate));
        /* exponential: 511 -> below 1 (ln 512 = 6.238) in 2.826 s */
        uint64_t k = ((408824ull * 1000u * BLOCK * frac) << sh)
                   / (2826ull * 4u * rate);
        atk_k[r] = (uint32_t)(k > 65536u ? 65536u : k);
    }
    lfo_am_inc  = (uint32_t)((37ull  << 32) * BLOCK / (10u * rate));
    lfo_vib_inc = (uint32_t)((61ull  << 32) * BLOCK / (10u * rate));

    for (unsigned c = 0; c < OPL_CHANNELS; c++) {
        chan_t *p = &ch[c];
        *p = (chan_t){ 0 };
        for (unsigned i = 0; i < 2; i++) {
            p->op[i].env = ENV_MAX;
            p->op[i].state = EG_OFF;
        }
    }
    wse = dam = dvb = 0;
    blk_left = 0;
}

void opl_write(unsigned reg, unsigned val)
{
    const unsigned set = reg >> 8;
    if (set >= OPL_CHANNELS / 9u) return;
    reg &= 0xFFu;
    val &= 0xFFu;

    if (reg == 0x01) { if (!set) wse = (val >> 5) & 1u; return; }
    if (reg == 0xBD) { if (!set) { dam = val >> 7; dvb = (val >> 6) & 1u; } return; }

    const unsigned grp = reg & 0xE0u;
    if (grp == 0x20 || grp == 0x40 || grp == 0x60 || grp == 0x80 ||
        grp == 0xE0) {
        const unsigned off = reg & 0x1Fu, row = off / 8u, col = off % 8u;
        if (col >= 6u || row >= 3u) return;
        chan_t *c = &ch[set * 9u + row * 3u + col % 3u];
        op_t *o = &c->op[col / 3u];

        switch (grp) {
        case 0x20:
            o->am = val >> 7; o->vib = (val >> 6) & 1u;
            o->egt = (val >> 5) & 1u; o->ksr = (val >> 4) & 1u;
            o->mult = val & 15u;
            op_freq(c, o);
            break;
        case 0x40: o->ksl = val >> 6; o->tl = val & 63u;        break;
        case 0x60: o->ar = val >> 4;  o->dr = val & 15u;        break;
        case 0x80: o->sl = val >> 4;  o->rr = val & 15u;        break;
        default:   o->ws = wse ? (val & 3u) : 0u;               break;
        }
        return;
    }

    const unsigned n = reg & 0x0Fu;
    if (n > 8u) return;
    chan_t *c = &ch[set * 9u + n];

    switch (reg & 0xF0u) {
    case 0xA0:
        c->fnum = (uint16_t)((c->fnum & 0x300u) | val);
        op_freq(c, &c->op[0]); op_freq(c, &c->op[1]);
        break;
    case 0xB0: {
        c->fnum  = (uint16_t)((c->fnum & 0xFFu) | ((val & 3u) << 8));
        c->block = (uint8_t)((val >> 2) & 7u);
        op_freq(c, &c->op[0]); op_freq(c, &c->op[1]);

        const unsigned key = (val >> 5) & 1u;
        for (unsigned i = 0; i < 2; i++) {
            op_t *o = &c->op[i];
            if (key && !c->key) {
                o->state = EG_ATTACK;
                o->phase = 0;
            } else if (!key && c->key && o->state != EG_OFF) {
                o->state = EG_RELEASE;
            }
        }
        c->key = (uint8_t)key;
        break;
    }
    case 0xC0:
        c->fb = (uint8_t)((val >> 1) & 7u);
        c->cnt = (uint8_t)(val & 1u);
        break;
    default:
        break;
    }
}

/* Envelopes and LFOs, once per BLOCK samples. */
static void tick_block(void)
{
    lfo_am  += lfo_am_inc;
    lfo_vib += lfo_vib_inc;

    /* Triangles: tremolo 0..255, vibrato -256..256. */
    unsigned t = lfo_am >> 23;
    t = t < 256u ? t : 511u - t;
    const unsigned am_att = (t * (dam ? 26u : 6u)) / 255u;

    int32_t v = (int32_t)(lfo_vib >> 22);           /* 0..1023 */
    v = v < 512 ? v - 256 : 768 - v;
    const int32_t vib_q16 = v * (dvb ? 531 : 265) / 256;  /* 14 / 7 cents */

    for (unsigned c = 0; c < OPL_CHANNELS; c++)
        for (unsigned i = 0; i < 2; i++)
            op_block(&ch[c].op[i], am_att, vib_q16);
}

static void render_chan(chan_t *c, int32_t *out, unsigned n)
{
    op_t *m = &c->op[0], *k = &c->op[1];
    const unsigned fb = c->fb, cnt = c->cnt;

    for (unsigned i = 0; i < n; i++) {
        const int32_t fbm = fb ? (m->out0 + m->out1) >> (9u - fb) : 0;
        const int32_t mo = (wave(m->ws, (m->phase >> 22) + (unsigned)fbm)
                            * m->gain) >> 12;
        m->phase += m->inc_now;
        m->out1 = m->out0;
        m->out0 = mo;

        const int32_t co = (wave(k->ws, (k->phase >> 22)
                                 + (unsigned)(cnt ? 0 : mo)) * k->gain) >> 12;
        k->phase += k->inc_now;

        out[i] += cnt ? mo + co : co;
    }
}

void opl_render(int32_t *out, unsigned n)
{
    while (n) {
        if (!blk_left) { tick_block(); blk_left = BLOCK; }
        const unsigned run = n < blk_left ? n : blk_left;

        for (unsigned c = 0; c < OPL_CHANNELS; c++) {
            chan_t *p = &ch[c];
            /* Silent channels cost nothing; their phases simply pause. */
            if (!p->op[0].gain && !p->op[1].gain) continue;
            render_chan(p, out, run);
        }
        out += run;
        n -= run;
        blk_left -= run;
    }
}
