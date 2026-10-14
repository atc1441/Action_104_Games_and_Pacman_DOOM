/*
 * board.h - Action PAC-MAN mini console (article 3219232)
 *
 * Same SoC as the 104-games console - the boot ROM is byte-identical, so
 * it is the same mask ROM - but a completely different application. The
 * stock firmware here is a licensed Bandai Namco arcade emulator
 * ("Featuring Moo Emulation", build V.289.1 Aug 12 2025) written in C++,
 * with none of the FlyThings/ZKSWE strings the other board has.
 *
 * Measured on the running stock firmware:
 *
 *   - buttons, by polling the GPIO input registers over SWD while each
 *     one was pressed (reads are harmless on this SoC; it is writes that
 *     wedge the debug bus)
 *   - pin and controller configuration, by dumping GPIO and SPI registers
 *     while the stock firmware was driving the display
 *
 * The panel init was recovered from the stock LCD init (docs/HARDWARE.md).
 *
 * The display pins are the same as on the 104-games board, and the SPI
 * controller is configured identically (mode 0x2881, RX control 0x100).
 */

#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include "soc.h"

#define BOARD_NAME "Action PAC-MAN"

/* ------------------------------------------------------------------ *
 * Display
 * ------------------------------------------------------------------ */
/*
 * Landscape. MADCTL sets the rotation and the geometry has to match it:
 * with MV the controller swaps rows and columns, so the window commands
 * address 320x240. Get the two out of step and the window runs off the
 * end of the panel.
 */
#define LCD_WIDTH   320
#define LCD_HEIGHT  240
#define LCD_BPP     16          /* RGB565 */

#define LCD_PORT      GPIOA_BASE
#define LCD_PIN_DC    1         /* LOW = command, HIGH = data */
#define LCD_PIN_CS    2
#define LCD_PIN_SCK   3
#define LCD_PIN_MOSI  4
#define LCD_PIN_BL    8         /* see the note below - this is RESET here */

/*
 * PA8 is the panel's RESET, not a backlight as on the 104-games board.
 *
 * The stock firmware pulses it low, waits, and releases it before
 * sending any command. lcd.c does the same when this is defined.
 */
#define BOARD_LCD_RESET_PIN 8

/* This panel ignores MADCTL, and it sits turned by 90 degrees in the
 * case, so the rotation is done in software when the frame is pushed
 * out. See i_system_console.c. */
#define BOARD_LCD_ROT90

/*
 * Controller configuration, from the running stock firmware. The pins are
 * set up in lcd.c.
 */
#define BOARD_LCD_SPI_MODE   0x00002881u
#define BOARD_LCD_SPI_RXCTL  0x00000100u

typedef struct {
    uint8_t cmd;
    uint8_t nargs;
    uint8_t args[10];
} lcd_cmd_t;

/*
 * The panel init, recovered from the stock firmware, where it is
 * unrolled code: single-byte writes to the SPI data register, with the DC
 * line toggled through GPIOA BSRR between commands and parameters.
 *
 * Same controller family as the 104-games board, different parameters:
 *
 *   MADCTL     0x28 in the stock init, 0xE8 there
 *   0x21       display inversion on; the other board does not send it
 *   E8/EC/ED   different timing values
 *   C3/C4/C9   different power values
 *   F0-F3      its own gamma curves
 */
static const lcd_cmd_t board_lcd_init_seq[] = {
    { 0xFE, 0, { 0 } },
    { 0xEF, 0, { 0 } },
    /*
     * MADCTL. The stock value is 0x28; with it the image comes out upside
     * down, because the stock firmware draws its own content the other way
     * round. Adding MY|MX turns it by 180 degrees.
     */
    { 0x36, 1, { 0xE8 } },                                  /* MADCTL      */
    { 0x3A, 1, { 0x05 } },                                  /* RGB565      */
    { 0x21, 0, { 0 } },                                     /* inversion on*/
    { 0x86, 1, { 0x98 } },
    { 0x89, 1, { 0x33 } },
    { 0x8B, 1, { 0x80 } },
    { 0x8D, 1, { 0x33 } },
    { 0x8E, 1, { 0x0F } },
    { 0xE8, 2, { 0x13, 0x55 } },
    { 0xEC, 3, { 0x33, 0x06, 0x00 } },
    { 0xED, 2, { 0x18, 0x09 } },
    { 0xFF, 9, { 0x62, 0x99, 0x3E, 0x9D, 0x4B,
                 0x98, 0x3E, 0x9C, 0x4B } },
    { 0xC3, 1, { 0x1A } },
    { 0xC4, 1, { 0x10 } },
    { 0xC9, 1, { 0x18 } },
    { 0xF0, 6, { 0x82, 0x00, 0x16, 0x15, 0x0B, 0x3C } },
    { 0xF1, 6, { 0x4D, 0x98, 0x98, 0x20, 0x2C, 0xBF } },
    { 0xF2, 6, { 0x42, 0x09, 0x0D, 0x0E, 0x0A, 0x3B } },
    { 0xF3, 6, { 0x4D, 0x93, 0x91, 0x20, 0x1A, 0x9F } },
    { 0x35, 1, { 0x00 } },                                  /* tearing on  */
    { 0x44, 2, { 0x00, 0x0A } },                            /* tear line   */
    { 0x11, 0, { 0 } },                                     /* sleep out   */
    { 0x29, 0, { 0 } },                                     /* display on  */
};

#define BOARD_LCD_INIT_SEQ_LEN \
    (sizeof(board_lcd_init_seq) / sizeof(board_lcd_init_seq[0]))

/* ------------------------------------------------------------------ *
 * Buttons - measured, eight for eight, no ambiguity
 *
 * The wiring is identical to the 104-games board: the same
 * pins carry the d-pad, and the same two carry A and B. This console just
 * populates fewer of them, and its VOL+/VOL- sit on two of the pins the
 * other board used for its volume button (PA0 / PB2 / PC13).
 *
 * VOL+ and VOL- become START and SELECT: DOOM needs those far more than
 * it needs volume control, and there is nothing else left to use.
 *
 * What this console does not have is the pair of turbo buttons that
 * became L and R on the 104-games board, so there is no dedicated strafe
 * key here.
 * ------------------------------------------------------------------ */
#define BOARD_NUM_KEYS 8

#define BOARD_KEY_TABLE                                                   \
    /*  port         pin  name    key           button on the case      */\
    {  { GPIOB_BASE,  7,  "PB7"  }, KEY_UP     },   /* d-pad up        */ \
    {  { GPIOB_BASE,  5,  "PB5"  }, KEY_DOWN   },   /* d-pad down      */ \
    {  { GPIOB_BASE,  6,  "PB6"  }, KEY_LEFT   },   /* d-pad left      */ \
    {  { GPIOB_BASE,  4,  "PB4"  }, KEY_RIGHT  },   /* d-pad right     */ \
    {  { GPIOB_BASE,  0,  "PB0"  }, KEY_A      },   /* A               */ \
    {  { GPIOA_BASE, 11,  "PA11" }, KEY_B      },   /* B               */ \
    {  { GPIOA_BASE,  0,  "PA0"  }, KEY_START  },   /* VOL+  -> START  */ \
    {  { GPIOB_BASE,  2,  "PB2"  }, KEY_SELECT },   /* VOL-  -> SELECT */

#endif /* BOARD_H */
