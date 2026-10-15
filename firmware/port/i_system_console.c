/*
 * i_system_console.c - GBADoom's platform layer for this console.
 *
 * GBADoom keeps everything hardware-dependent behind a small set of
 * functions ending in _e32. This file implements them; the engine itself
 * needs no changes beyond the handful of patches listed in
 * docs/PORTING.md.
 *
 * Frame format: GBADoom writes its framebuffer as unsigned short, i.e.
 * two 8-bit palette indices per 16-bit word. SCREENWIDTH = 120 therefore
 * means 240 real pixels, and the image is 240 x 160. The panel is
 * 320 x 240, of which the case shows about 266 x 218, and the frame is
 * scaled to the width of that (see Geometry).
 *
 * The WAD is NOT in RAM. w_wad.c reaches into it directly via
 * &doom_iwad[filepos], and here doom_iwad points into the XIP window of
 * the external flash - exactly as it points into cartridge ROM on a GBA.
 * That is precisely why this port fits and the PSRAM-based ones do not.
 */

#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "doomtype.h"
#include "i_system_e32.h"
#include "d_event.h"
#include "d_main.h"
#include "doomdef.h"

#include "soc.h"
#include "lcd.h"
#include "input.h"
#include "hostbox.h"
#include "clock.h"
#include "board.h"
#include "uart.h"

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */
/*
 * The case covers the panel's edges: a test pattern showed about columns
 * 26-291 and rows 12-228, give or take 3 pixels. The 240 x 160 frame is
 * scaled, nearest neighbour, to 264 x 198 centred on the panel: 11/10
 * across to fill the visible width, and 4:3 overall, the shape the DOOM
 * art was drawn for. Building with -DFRAME_KEEP_ASPECT keeps the GBA's
 * 3:2 shape instead, 264 x 176.
 */
#define DOOM_W   240           /* real pixels, = 2 * SCREENWIDTH */
#define DOOM_H   160
#define OUT_W    264
#ifdef FRAME_KEEP_ASPECT
#define OUT_H    176
#else
#define OUT_H    198
#endif
#define ORIGIN_X ((LCD_WIDTH  - OUT_W) / 2)
#define ORIGIN_Y ((LCD_HEIGHT - OUT_H) / 2)

/* Source pixel for output pixel i of n, sampled at the pixel centre. */
#define SRC_OF(i, n, src) ((unsigned)(((2u * (i) + 1u) * (src)) / (2u * (n))))

/* ------------------------------------------------------------------ *
 * Palette: DOOM hands over 256 RGB888 entries, the panel wants RGB565
 * ------------------------------------------------------------------ */
/*
 * Stored byte-swapped: the panel wants the high byte first, and a 16-bit
 * store on this little-endian core then puts it first in memory.
 */
static uint16_t pal565[256];

/* Two rows, so one can be filled while DMA sends the other. */
static uint16_t row_rgb[2][OUT_W];

/* Source column of each output column, filled by I_InitScreen_e32. */
static uint8_t col_src[OUT_W];

void I_SetPallete_e32(const byte *pallete)
{
    if (!pallete) return;

    for (int i = 0; i < 256; i++) {
        unsigned r = *pallete++;
        unsigned g = *pallete++;
        unsigned b = *pallete++;
        const uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        pal565[i] = (uint16_t)((c >> 8) | (c << 8));
    }
}

/* ------------------------------------------------------------------ *
 * Credit overlay
 *
 * Painted into the row buffer on its way to the panel, not into the
 * framebuffer: the engine never sees it, so it cannot be overdrawn,
 * cleared by a palette change or wiped by the menu. It also costs nothing
 * when the row is outside the text band.
 *
 * 5x7 font, column-major, bit 0 = top row. Only the glyphs actually
 * needed plus a full A-Z and 0-9, which is small enough not to matter and
 * saves the next person from adding them.
 * ------------------------------------------------------------------ */
/* Top right, clear of the engine's messages, which start on the left. */
#define OVL_X   (DOOM_W - 3u - (sizeof(ovl_text) - 1u) * OVL_ADV)
#define OVL_Y   3
#define OVL_H   7
#define OVL_ADV 6                       /* 5 px glyph + 1 px gap */

static const char ovl_text[] = "by ATC1441";

static const uint8_t font_digit[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
};

static const uint8_t font_upper[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};

/* Lowercase is not worth a full second table for two letters. */
static const uint8_t font_b[5] = {0x7F,0x48,0x44,0x44,0x38};
static const uint8_t font_y[5] = {0x0C,0x50,0x50,0x50,0x3C};

static const uint8_t *glyph(char c)
{
    if (c == 'b') return font_b;
    if (c == 'y') return font_y;
    if (c >= '0' && c <= '9') return font_digit[c - '0'];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return font_upper[c - 'A'];
    return 0;                            /* space and anything unknown */
}

/*
 * Is this source pixel part of the credit overlay?
 * Returns -1 for "no", 0 for the black plate, 1 for the text itself.
 *
 * Asked per pixel in source coordinates rather than painted into the
 * output row, so that it comes out right whichever way the frame is
 * rotated on its way to the panel.
 */
static int overlay_pixel(unsigned x, unsigned y)
{
    const unsigned n = sizeof(ovl_text) - 1;

    if (y + 1 < OVL_Y || y > OVL_Y + OVL_H) return -1;
    if (x + 1 < OVL_X || x > OVL_X + n * OVL_ADV) return -1;

    if (y < OVL_Y || y >= OVL_Y + OVL_H) return 0;
    if (x < OVL_X) return 0;

    const unsigned i = (x - OVL_X) / OVL_ADV;
    const unsigned c = (x - OVL_X) % OVL_ADV;
    if (i >= n || c >= 5) return 0;

    const uint8_t *g = glyph(ovl_text[i]);
    if (!g) return 0;
    return (g[c] & (1u << (y - OVL_Y))) ? 1 : 0;
}

/* Can source row y / column x touch the overlay? Same bounds as above. */
static int ovl_row(unsigned y)
{
    return !(y + 1 < OVL_Y || y > OVL_Y + OVL_H);
}

static int ovl_col(unsigned x)
{
    return !(x + 1 < OVL_X || x > OVL_X + (sizeof(ovl_text) - 1) * OVL_ADV);
}

/* Output pixel for an overlay_pixel() result and the source index. */
static uint16_t ovl_rgb(int ov, byte idx)
{
    if (ov < 0)  return pal565[idx];
    if (ov == 0) return 0x0000;
    return 0xFFFF;
}

/* ------------------------------------------------------------------ *
 * Video output
 *
 * The frame goes to the panel in the background. I_FinishUpdate_e32
 * converts the first two rows and starts the first; from then on the DMA
 * interrupt at the end of each row starts the next one and converts the
 * one after it into the buffer just freed. The engine meanwhile runs its
 * tics, and only waits in I_GetBackBuffer, before it draws again.
 * ------------------------------------------------------------------ */
#ifdef BOARD_LCD_ROT90
/*
 * Quarter turn the other way, done here because this board's panel
 * ignores MADCTL. Swapping which source coordinate is mirrored is what
 * turns it by a further 180 degrees.
 * The image becomes 160 wide and 240 tall, which fills the short edge
 * exactly. Reading down a source column instead of along a row costs a
 * strided access per pixel and no extra memory.
 */
#define PUSH_ROWS  DOOM_W
#define PUSH_LEN   DOOM_H
#else
#define PUSH_ROWS  OUT_H
#define PUSH_LEN   OUT_W
#endif

static const byte *push_src;           /* frame being sent               */
static unsigned push_row;              /* row on the SPI now             */
static volatile int push_busy;

/* Output row y of push_src, as RGB565 ready for the panel. */
static void push_convert(unsigned y, uint16_t *d)
{
#ifdef BOARD_LCD_ROT90
    const unsigned sx = DOOM_W - 1u - y;  /* one source column per row */
    const byte *s = push_src + sx;

    if (ovl_col(sx)) {
        for (unsigned x = 0; x < DOOM_H; x++)
            d[x] = ovl_rgb(overlay_pixel(sx, x), s[x * DOOM_W]);
    } else {
        for (unsigned x = 0; x < DOOM_H; x++)
            d[x] = pal565[s[x * DOOM_W]];
    }
#else
    /* A source row shown twice is simply converted twice. */
    const unsigned sy = SRC_OF(y, OUT_H, DOOM_H);
    const byte *s = push_src + sy * DOOM_W;

    if (ovl_row(sy)) {
        for (unsigned x = 0; x < OUT_W; x++) {
            const unsigned sx = col_src[x];
            d[x] = ovl_rgb(overlay_pixel(sx, sy), s[sx]);
        }
    } else {
        for (unsigned x = 0; x < OUT_W; x++)
            d[x] = pal565[s[col_src[x]]];
    }
#endif
}

/* End of a row on the DMA side: start the next, convert the one after. */
void DMA_CH2_IRQHandler(void)
{
#ifdef PROFILE
    extern volatile uint32_t prof_push_cyc;
    const uint32_t prof_t0 = DWT_CYCCNT;
#endif
    lcd_data_wait();

    const unsigned y = ++push_row;
    if (y < PUSH_ROWS) {
        lcd_data_start_irq((const uint8_t *)row_rgb[y & 1u], PUSH_LEN * 2u);
        if (y + 1u < PUSH_ROWS)
            push_convert(y + 1u, row_rgb[(y + 1u) & 1u]);
    } else {
        push_busy = 0;
    }
#ifdef PROFILE
    prof_push_cyc += DWT_CYCCNT - prof_t0;
#endif
}

/* Until the frame being sent is out, the framebuffer must not change. */
static void push_wait(void)
{
#ifdef PROFILE
    /* Less the interrupts taken meanwhile, which have their own counters. */
    extern volatile uint32_t prof_push_wait_cyc, prof_audio_cyc, prof_push_cyc;
    const uint32_t prof_t0 = DWT_CYCCNT;
    const uint32_t prof_irq0 = prof_audio_cyc + prof_push_cyc;
    while (push_busy) { }
    prof_push_wait_cyc += DWT_CYCCNT - prof_t0
                        - (prof_audio_cyc + prof_push_cyc - prof_irq0);
#else
    while (push_busy) { }
#endif
}

void I_FinishUpdate_e32(const byte *srcBuffer, const byte *pallete,
                        const unsigned int width, const unsigned int height)
{
    if (!srcBuffer) return;
    push_wait();
#ifdef PROFILE
    /* Frames pushed and cycles spent pushing them, read over SWD. */
    extern volatile uint32_t prof_frames, prof_push_cyc;
    const uint32_t prof_t0 = DWT_CYCCNT;
    prof_frames++;
#endif
    if (pallete) I_SetPallete_e32(pallete);

#ifdef BOARD_LCD_ROT90
    const unsigned ox = (LCD_WIDTH  - DOOM_H) / 2u;
    const unsigned oy = (LCD_HEIGHT - DOOM_W) / 2u;
    lcd_set_window(ox, oy, ox + DOOM_H - 1u, oy + DOOM_W - 1u);
#else
    lcd_set_window(ORIGIN_X, ORIGIN_Y,
                   ORIGIN_X + OUT_W - 1u, ORIGIN_Y + OUT_H - 1u);
#endif

    push_src  = srcBuffer;
    push_row  = 0;
    push_busy = 1;
    push_convert(0, row_rgb[0]);
    push_convert(1, row_rgb[1]);
    lcd_data_start_irq((const uint8_t *)row_rgb[0], PUSH_LEN * 2u);
#ifdef PROFILE
    prof_push_cyc += DWT_CYCCNT - prof_t0;
#endif
}

#ifdef PROFILE
/*
 * PROFILE builds only: cycle counters that tools/profile.py reads over
 * SWD. prof_audio_cyc lives in sdk/audio.c, prof_spi_wait_cyc is added to
 * in sdk/lcd.c and prof_idle_cyc in GBADoom's TryRunTics.
 */
volatile uint32_t prof_frames, prof_push_cyc, prof_idle_cyc, prof_spi_wait_cyc,
                  prof_push_wait_cyc;
#endif

int  I_GetVideoWidth_e32(void)  { return 120; }   /* in 16-bit words */
int  I_GetVideoHeight_e32(void) { return 160; }

void I_InitScreen_e32(void)
{
    for (unsigned x = 0; x < OUT_W; x++)
        col_src[x] = (uint8_t)SRC_OF(x, OUT_W, DOOM_W);

    lcd_init();
    lcd_fill(0x0000);
    NVIC_ISER0 = 1u << DMA_CH2_IRQn;          /* frame push, see Video output */
    input_init();
    input_scan_init();
    /* input_init() puts PB0 back to GPIO in; claim USART2 pins again. */
    uart_init();
}

/* On the GBA these flip the VRAM page; we draw straight to the panel. */
void I_BlitScreenBmp_e32(void)   { }
void I_CreateWindow_e32(void)    { }
void I_CreateBackBuffer_e32(void){ }

void I_ClearWindow_e32(void)     { push_wait(); lcd_fill(0x0000); }

/* ------------------------------------------------------------------ *
 * Time - TICRATE is 35 Hz
 *
 * Accumulate cycles and convert to tics WITHOUT dividing.
 *
 * The engine calls I_GetTime from tight loops, and a 64-bit division
 * (__aeabi_uldivmod) per call dominates the profile. This only subtracts
 * and compares, all in 32 bits, and the while loop normally runs zero or
 * one times.
 *
 * The counter is 32 bits wide and wraps after about 22 seconds at 194 MHz
 * (about 69 seconds at 62 MHz), which is exactly why this accumulates
 * instead of dividing the absolute value.
 * ------------------------------------------------------------------ */
#define TICRATE_HZ 35u

static uint32_t cycles_per_tic = 62200000u / TICRATE_HZ;
static uint32_t last_cyc;
static uint32_t acc_cyc;
static int      tic_count;

void I_TimerInit_console(uint32_t core_hz)
{
    cycles_per_tic = core_hz / TICRATE_HZ;
    last_cyc  = DWT_CYCCNT;
    acc_cyc   = 0;
    tic_count = 0;
}

int I_GetTime_e32(void)
{
    uint32_t now = DWT_CYCCNT;

    acc_cyc += (uint32_t)(now - last_cyc);   /* wrapping is correct here */
    last_cyc = now;

    while (acc_cyc >= cycles_per_tic) {
        acc_cyc -= cycles_per_tic;
        tic_count++;
    }
    return tic_count;
}

/* ------------------------------------------------------------------ *
 * Input
 * ------------------------------------------------------------------ */
void I_StartWServEvents_e32(void) { }

static uint16_t keys_prev;

/*
 * The engine calls I_ProcessKeyEvents (see I_StartTic in i_video.c), NOT
 * I_PollWServEvents_e32 - the latter stays empty.
 *
 * Besides the real buttons this also reads a bit mask from the host
 * mailbox, which can be set over SWD. That makes the game fully drivable
 * from a debugger - menu, level start, movement - which is how it was
 * brought up before the button map was known. Harmless to leave in: the
 * magic word is cleared at startup, so it does nothing unless a host
 * writes to it.
 */
void I_ProcessKeyEvents(void)
{
    uint16_t now = 0;

    /* mirror the input registers into the host mailbox, for working out
     * a new board's button map - see docs/PORTING.md */
    input_publish();

    if (g_hostbox.key_magic == HOSTBOX_KEY_MAGIC) {
        now = (uint16_t)g_hostbox.keys;
    }

    /*
     * Add the real buttons. The engine's bit order is KEYD_A + i for
     * i = 0..9; the SDK's is the NES joypad order. Hence an explicit
     * table rather than a silent assumption that the two happen to line
     * up. The two turbo buttons become L and R, which DOOM uses for
     * strafing and weapon switching.
     */
    static const uint16_t hw_to_doom[10][2] = {
        { KEY_A,       1u << 0 },   /* A      */
        { KEY_B,       1u << 1 },   /* B      */
        { KEY_TURBO_A, 1u << 2 },   /* L      */
        { KEY_TURBO_B, 1u << 3 },   /* R      */
        { KEY_UP,      1u << 4 },
        { KEY_DOWN,    1u << 5 },
        { KEY_LEFT,    1u << 6 },
        { KEY_RIGHT,   1u << 7 },
        { KEY_START,   1u << 8 },
        { KEY_SELECT,  1u << 9 },
    };
    const uint32_t hw = input_read();
    for (int i = 0; i < 10; i++) {
        if (hw & hw_to_doom[i][0]) { now |= hw_to_doom[i][1]; }
    }

    uint16_t changed = now ^ keys_prev;
    for (int i = 0; i < 10; i++) {
        if (changed & (1u << i)) {
            event_t ev;
            ev.type = (now & (1u << i)) ? ev_keydown : ev_keyup;
            ev.data1 = KEYD_A + i;      /* KEYD_A..KEYD_SELECT are 1..10 */
            D_PostEvent(&ev);
        }
    }
    keys_prev = now;
}

void I_Quit_e32(void)
{
    for (;;) { }
}

/*
 * Full system reset.
 *
 * Used when a finished level has no successor in the trimmed WAD (see the
 * patch in GBADoom/source/g_game.c). SYSRESETREQ takes the whole chip
 * down, the boot ROM runs, re-initialises the flash controller and
 * jumps back to 0x08004000 - so the player lands on the
 * title screen with everything in a known state, which a software restart
 * of the engine would not give us.
 *
 * The short spin before the reset is not decoration: the request is
 * asynchronous, and without DSB the write can still be sitting in the
 * write buffer while the core runs on.
 */
void I_Reboot_console(void)
{
    __asm volatile ("cpsid i" ::: "memory");
    __asm volatile ("dsb sy" ::: "memory");
    SCB_AIRCR = 0x05FA0004u;      /* VECTKEY | SYSRESETREQ */
    __asm volatile ("dsb sy" ::: "memory");
    for (;;) { }
}

/* ------------------------------------------------------------------ *
 * Entry point
 *
 * Our startup calls main(). GBADoom's own main() is renamed to
 * doom_main() at compile time (see the Makefile) so that the clock and
 * peripherals can be brought up first.
 * ------------------------------------------------------------------ */
extern int doom_main(int argc, const char *const *argv);

int main(void)
{
    clock_init();

    /* Discard a stale error message - SRAM survives a reset, so one left
     * over from the previous run would look current. */
    g_hostbox.err_magic  = 0;
    g_hostbox.key_magic  = 0;
    g_hostbox.scan_magic = 0;

    I_TimerInit_console(g_cpu_hz);
    uart_init();
    return doom_main(0, 0);
}

/* ------------------------------------------------------------------ *
 * Framebuffers
 *
 * On the GBA these point straight into VRAM. Here they are ordinary RAM
 * buffers that I_FinishUpdate_e32 pushes to the panel over SPI.
 *
 * Front and back buffer deliberately share the same memory: two separate
 * ones cost 2 x 38.4 KB, and the screen wipe is not worth that while the
 * heap is tight. The only visible consequence is a scruffy transition
 * effect between menu and game.
 * ------------------------------------------------------------------ */
#define FB_SHORTS (120u * 160u)

static unsigned short framebuffer[FB_SHORTS];

/* Both wait for the frame being sent: the engine draws right after. */
unsigned short *I_GetBackBuffer(void)  { push_wait(); return framebuffer; }
unsigned short *I_GetFrontBuffer(void) { push_wait(); return framebuffer; }

/* ------------------------------------------------------------------ *
 * Fatal error
 *
 * The message goes to USART2 and to a fixed SRAM address so it can be
 * read over SWD if the UART is dead. The screen turns red, then the core
 * spins (no wfi - see Default_Handler in startup.c).
 * ------------------------------------------------------------------ */
void I_Error(const char *error, ...)
{
    va_list ap;

    va_start(ap, error);
    vsnprintf((char *)g_hostbox.err_text, sizeof g_hostbox.err_text, error, ap);
    va_end(ap);

    g_hostbox.err_magic = HOSTBOX_ERR_MAGIC;

    uart_puts("I_Error: ");
    uart_puts((const char *)g_hostbox.err_text);
    uart_puts("\n");

    /* Stop the frame push; while it runs, a row is always on the SPI. */
    NVIC_ICER0 = 1u << DMA_CH2_IRQn;
    if (push_busy) {
        lcd_data_wait();
        push_busy = 0;
    }
    lcd_fill(0xF800);          /* red */
    for (;;) { }
}

void I_PollWServEvents_e32(void) { }
