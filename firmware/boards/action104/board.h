/*
 * board.h - Action "104 Games" mini game console (article 3217060)
 *
 * Everything in here is specific to this board. The SDK sources
 * (sdk/lcd.c, sdk/input.c) contain no board constants of their own; they
 * pull them from this header. Adding another board that uses the same SoC
 * means writing one more of these - see boards/pacman/board.h.
 *
 * Every value below was read out of the stock firmware or measured on the
 * device. None of it is guesswork, and where something is still unproven
 * it says so.
 */

#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include "soc.h"

#define BOARD_NAME "Action 104 Games"

/* ------------------------------------------------------------------ *
 * Display
 *
 * 320x240 panel, RGB565. The controller answers a Galaxycore-style
 * command set (GC93xx / NV30xx family): 0x8x inner registers, 0xC3/0xC4/
 * 0xC9 power control, 0xE8/0xEC/0xED timing, 0xF0-0xF3 gamma. The exact
 * part was never identified and does not matter for the driver - the byte
 * sequence below is simply what the stock firmware sends.
 * ------------------------------------------------------------------ */
#define LCD_WIDTH   320
#define LCD_HEIGHT  240
#define LCD_BPP     16          /* RGB565 */

#define LCD_PORT      GPIOA_BASE
#define LCD_PIN_DC    1         /* LOW = command, HIGH = data */
#define LCD_PIN_CS    2
#define LCD_PIN_SCK   3
#define LCD_PIN_MOSI  4
#define LCD_PIN_BL    8         /* backlight, active HIGH */

/*
 * Pin and controller configuration as it stands while the stock firmware
 * runs. Read off the live device, not derived.
 *
 * The alternate-function numbers are why an early attempt hung forever:
 * putting a pin into AF mode is not enough, you also have to say WHICH
 * function. Without AFRL nothing reaches SCK/MOSI and the "TX done" wait
 * loop never finishes.
 */
#define BOARD_LCD_MODER      0x00010294u   /* pins 0..5 + PA8 (backlight) */
#define BOARD_LCD_MODER_MASK 0x00033FFFu
#define BOARD_LCD_AFRL       0xC03C0000u   /* PA4 -> AF12, PA5 -> AF3     */
#define BOARD_LCD_PUPDR      0xA5540555u
#define BOARD_LCD_SPI_MODE   0x00002881u   /* +0x08: bits 0 and 7 required */
#define BOARD_LCD_SPI_RXCTL  0x00000100u   /* +0x10 bit 8 - the same bit the
                                            * stock SRAM stub sets on the
                                            * flash controller             */

/*
 * Panel init, reconstructed from the stock firmware
 * (flash 0x0802FE06 .. 0x08032492). In the original the sequence is fully
 * unrolled - roughly 0x84 bytes of code per transferred byte, including
 * delay and status polling. Here it is one table.
 *
 * Note the first four bytes: FE, EF, 36 E8. An earlier extraction started
 * at 0x08030000 and cut them off, which lost MADCTL - the panel came up
 * rotated and in RGB instead of BGR.
 *
 * Raw byte sequence, exactly the order the stock firmware sends (75 bytes):
 *
 *   FE  EF  36 E8
 *   3A 05  86 98  89 33  8B 80  8D 33  8E 0F
 *   E8 11 33  EC 33 00 1F  ED 10 08  E8 12 00
 *   C3 16  C4 25  C9 28
 *   FF 62 99 3E 9D 4B 98 3E 9C 4B
 *   F0 02 08 08 08 04 2B
 *   F1 42 71 73 2C 30 6F
 *   F2 02 08 08 08 04 2B
 *   F3 42 71 73 2C 30 6F
 *   11  29
 *
 * The window commands (0x2A/0x2B/0x2C) are NOT part of this sequence; the
 * stock code sets them in its fill routine at 0x0800586C.
 */
typedef struct {
    uint8_t cmd;
    uint8_t nargs;
    uint8_t args[10];
} lcd_cmd_t;

static const lcd_cmd_t board_lcd_init_seq[] = {
    { 0xFE, 0, { 0 } },                                     /* inter register       */
    { 0xEF, 0, { 0 } },                                     /* enable (GC9xxx)      */
    { 0x36, 1, { 0xE8 } },                                  /* MADCTL: MY|MX|MV|BGR */
    { 0x3A, 1, { 0x05 } },                                  /* COLMOD: RGB565       */
    { 0x86, 1, { 0x98 } },
    { 0x89, 1, { 0x33 } },
    { 0x8B, 1, { 0x80 } },
    { 0x8D, 1, { 0x33 } },
    { 0x8E, 1, { 0x0F } },
    { 0xE8, 2, { 0x11, 0x33 } },
    { 0xEC, 3, { 0x33, 0x00, 0x1F } },
    { 0xED, 2, { 0x10, 0x08 } },
    { 0xE8, 2, { 0x12, 0x00 } },                            /* 0xE8 appears twice   */
    { 0xC3, 1, { 0x16 } },
    { 0xC4, 1, { 0x25 } },
    { 0xC9, 1, { 0x28 } },
    { 0xFF, 9, { 0x62, 0x99, 0x3E, 0x9D, 0x4B,
                 0x98, 0x3E, 0x9C, 0x4B } },                /* command set unlock   */
    { 0xF0, 6, { 0x02, 0x08, 0x08, 0x08, 0x04, 0x2B } },    /* gamma 1              */
    { 0xF1, 6, { 0x42, 0x71, 0x73, 0x2C, 0x30, 0x6F } },    /* gamma 2              */
    { 0xF2, 6, { 0x02, 0x08, 0x08, 0x08, 0x04, 0x2B } },    /* gamma 3              */
    { 0xF3, 6, { 0x42, 0x71, 0x73, 0x2C, 0x30, 0x6F } },    /* gamma 4              */
    { 0x11, 0, { 0 } },                                     /* sleep out            */
    { 0x29, 0, { 0 } },                                     /* display on           */
};

#define BOARD_LCD_INIT_SEQ_LEN \
    (sizeof(board_lcd_init_seq) / sizeof(board_lcd_init_seq[0]))

/* ------------------------------------------------------------------ *
 * Buttons
 *
 * Taken from the stock key scan (app_input_poll, flash 0x0803F080; the
 * dense run of IDR reads starting at 0x08041842). It builds a bit mask
 * and stores it at 0x20030329; the debug print next to it reads
 * "get key[%02x]". Each test is LSLS Rx,#n followed by PL or EQ, which
 * means bit 31-n, active low.
 *
 * The bit order is that of an NES joypad (A, B, Select, Start, Up, Down,
 * Left, Right) - no coincidence on a console whose stock application is
 * an NES emulator ("checkNESMagic: skipped rom with invalid mapper #%d").
 *
 * Then verified on the device by pressing each button and watching the
 * mirrored input registers: ten distinct pins, all from the set derived
 * above, and the d-pad landed exactly on the direction bits.
 *
 * Not game buttons:
 *   PB3   - bit 2 ("Select" in the NES byte) is not populated on this
 *           board; it never moved under any press. MENU takes that role.
 *   PA0, PB2, PC13 - three-position volume slider, handled by a separate
 *           RAM-resident routine that prints "AudioVolume:%d".
 *
 * The two turbo buttons (PC6, PA12) are OR'ed onto A and B by the stock
 * firmware. Here they get keys of their own: DOOM has better uses for two
 * extra buttons than duplicating fire.
 */
#define BOARD_NUM_KEYS 11

#define BOARD_KEY_TABLE                                                  \
    /*  port         pin  name    key          button on the case      */\
    {  { GPIOB_BASE,  0,  "PB0"  }, KEY_A       },  /* right button 4 (bottom) */ \
    {  { GPIOC_BASE,  6,  "PC6"  }, KEY_TURBO_A },  /* right button 2          */ \
    {  { GPIOA_BASE, 11,  "PA11" }, KEY_B       },  /* right button 3          */ \
    {  { GPIOA_BASE, 12,  "PA12" }, KEY_TURBO_B },  /* right button 1 (top)    */ \
    {  { GPIOB_BASE,  3,  "PB3"  }, 0           },  /* not populated           */ \
    {  { GPIOC_BASE,  8,  "PC8"  }, KEY_START   },  /* START                   */ \
    {  { GPIOB_BASE,  7,  "PB7"  }, KEY_UP      },  /* d-pad up                */ \
    {  { GPIOB_BASE,  5,  "PB5"  }, KEY_DOWN    },  /* d-pad down              */ \
    {  { GPIOB_BASE,  6,  "PB6"  }, KEY_LEFT    },  /* d-pad left              */ \
    {  { GPIOB_BASE,  4,  "PB4"  }, KEY_RIGHT   },  /* d-pad right             */ \
    {  { GPIOC_BASE,  7,  "PC7"  }, KEY_SELECT  },  /* MENU                    */

#endif /* BOARD_H */
