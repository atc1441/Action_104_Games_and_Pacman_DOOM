/*
 * lcd.c - display driver, rebuilt from the stock firmware
 *
 * The stock firmware sends every byte through a fully unrolled sequence
 * (app 0x0803000A..0x08032492). This is the same sequence, written as a
 * loop. Board-specific pins, register values and the panel init table
 * come from boards/<board>/board.h.
 *
 * On CS handling: a command and its parameters belong in ONE transaction
 * with CS held low throughout. See lcd_cmd_args() for why that matters
 * more than it looks.
 */

#include "lcd.h"
#include "soc.h"
#include "board.h"

#define SPI SPI_LCD_BASE

/* Coarse busy-wait. At ~62 MHz this lands roughly on the requested
 * milliseconds. It is not exact and does not need to be. */
void lcd_delay_ms(uint32_t ms)
{
    volatile uint32_t n = ms * 12000u;
    while (n--) { __asm volatile ("nop"); }
}

/*
 * One transfer of arbitrary length, CS stays low throughout.
 *
 * The order is exactly the bootloader's SPI routine (0x2B60): set the
 * length once, start, then push bytes while the TX FIFO has room. A first
 * version did this whole dance per byte and managed about 5 frames per
 * second.
 */
#define SR_TXFULL  (1u << 3)
#define SR_TXDONE  (1u << 14)

static void spi_write(const uint8_t *buf, uint32_t len)
{
    if (!len) return;

    SPI_SR(SPI)   |= SR_TXDONE;
    SPI_SR(SPI)   |= 2u;
    SPI_CR0C(SPI) |= 2u;
    SPI_CR0C(SPI) &= ~2u;
    SPI_CR20(SPI)  = len;
    SPI_CR0C(SPI) |= 1u;
    SPI_CR24(SPI) |= 1u;

    for (uint32_t i = 0; i < len; i++) {
        while (SPI_SR(SPI) & SR_TXFULL) { }
        SPI_DR(SPI) = buf[i];
    }
    while (!(SPI_SR(SPI) & SR_TXDONE)) { }

    SPI_SR(SPI)   |= SR_TXDONE;
    SPI_SR(SPI)   |= 2u;
    SPI_CR0C(SPI) &= ~1u;
    SPI_CR24(SPI) &= ~1u;
}

void lcd_cmd(uint8_t c)
{
    lcd_cmd_args(c, 0, 0);
}

/*
 * Command plus parameters in ONE transaction - CS stays low, only DC
 * toggles.
 *
 * This is not cosmetic. An earlier version pulsed CS per byte, so no
 * parameter ever arrived. It went unnoticed for a long time because the
 * stock firmware had already configured the panel before we flashed over
 * it: the picture looked right while our own init was in fact doing
 * nothing. Only after a power cycle did the panel come up in its default
 * orientation with RGB instead of BGR.
 *
 * The stock window routine (0x08005968) does it exactly this way.
 */
void lcd_cmd_args(uint8_t c, const uint8_t *args, uint32_t n)
{
    GPIO_CLR(LCD_PORT, LCD_PIN_CS);

    GPIO_CLR(LCD_PORT, LCD_PIN_DC);      /* LOW = command */
    spi_write(&c, 1);

    if (n) {
        GPIO_SET(LCD_PORT, LCD_PIN_DC);  /* HIGH = data */
        spi_write(args, n);
    }

    GPIO_SET(LCD_PORT, LCD_PIN_CS);
}

void lcd_data(uint8_t d)
{
    lcd_data_buf(&d, 1);
}

void lcd_data_buf(const uint8_t *p, uint32_t n)
{
    GPIO_CLR(LCD_PORT, LCD_PIN_CS);
    GPIO_SET(LCD_PORT, LCD_PIN_DC);      /* HIGH = data */
    spi_write(p, n);
    GPIO_SET(LCD_PORT, LCD_PIN_CS);
}

void lcd_gpio_init(void)
{
    /* Clocks - exactly the bits the stock firmware sets */
    RCC_EN0 |= RCC_EN0_LCDSPI;
    RCC_EN1 |= RCC_EN1_GPIOA;
    RCC_EN2 |= RCC_EN2_LCDMISC;

    /* DC and CS as outputs, SCK and MOSI as alternate function */
    uint32_t m = GPIO_MODER(LCD_PORT);
    m &= ~BOARD_LCD_MODER_MASK;
    m |=  BOARD_LCD_MODER;
    GPIO_MODER(LCD_PORT) = m;

    GPIO_AFRL(LCD_PORT)  = BOARD_LCD_AFRL;
    GPIO_PUPDR(LCD_PORT) = BOARD_LCD_PUPDR;

    /* Controller base configuration */
    REG32(SPI + 0x08) = BOARD_LCD_SPI_MODE;
    REG32(SPI + 0x10) = BOARD_LCD_SPI_RXCTL;

    GPIO_SET(LCD_PORT, LCD_PIN_CS);
    GPIO_SET(LCD_PORT, LCD_PIN_DC);
    GPIO_SET(LCD_PORT, LCD_PIN_BL);      /* backlight on */
}

void lcd_init(void)
{
    lcd_gpio_init();
    lcd_delay_ms(20);

    /*
     * Hardware reset of the panel controller, for boards that wire one.
     *
     * Without it a cold start leaves the controller in an undefined state:
     * it lights up, accepts the init, and still shows nothing but a white
     * raster. That failure is easy to misread, because as long as some
     * other firmware has already initialised the panel - and flashing does
     * not cut power - everything looks fine.
     */
#ifdef BOARD_LCD_RESET_PIN
    GPIO_CLR(LCD_PORT, BOARD_LCD_RESET_PIN);
    lcd_delay_ms(20);
    GPIO_SET(LCD_PORT, BOARD_LCD_RESET_PIN);
    lcd_delay_ms(120);
#endif

    /*
     * A board may define BOARD_LCD_NO_INIT to skip the sequence entirely.
     * That is a bring-up aid, not a normal setting: flashing does not cut
     * power, so the panel keeps whatever the stock firmware configured.
     * If a correct picture appears without our init, the pins and the SPI
     * setup are right and only the sequence is wrong - which separates
     * two failures that otherwise look identical.
     */
#ifndef BOARD_LCD_NO_INIT
    for (uint32_t i = 0; i < BOARD_LCD_INIT_SEQ_LEN; i++) {
        const lcd_cmd_t *e = &board_lcd_init_seq[i];
        lcd_cmd_args(e->cmd, e->args, e->nargs);
        /* Sleep Out needs time before Display On may follow */
        if (e->cmd == 0x11) { lcd_delay_ms(120); }
    }
#endif
    lcd_delay_ms(20);
}

/*
 * Set the address window.
 *
 * 0x2A/0x2B/0x2C are NOT part of the stock init sequence - the stock code
 * issues them from its fill routine at 0x0800586C. The order used here is
 * the industry-standard one and works on this panel.
 */
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t a[4];

    a[0] = (uint8_t)(x0 >> 8); a[1] = (uint8_t)x0;
    a[2] = (uint8_t)(x1 >> 8); a[3] = (uint8_t)x1;
    lcd_cmd_args(0x2A, a, 4);

    a[0] = (uint8_t)(y0 >> 8); a[1] = (uint8_t)y0;
    a[2] = (uint8_t)(y1 >> 8); a[3] = (uint8_t)y1;
    lcd_cmd_args(0x2B, a, 4);

    lcd_cmd(0x2C);   /* RAMWR */
}

/* One screen line as a buffer - the stock application does the same,
 * keeping its line buffer at 0x20044BEC. */
static uint8_t line_buf[LCD_WIDTH * 2];

void lcd_fill(uint16_t color)
{
    for (uint32_t x = 0; x < LCD_WIDTH; x++) {
        line_buf[x * 2]     = (uint8_t)(color >> 8);
        line_buf[x * 2 + 1] = (uint8_t)(color);
    }

    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    for (uint32_t y = 0; y < LCD_HEIGHT; y++) {
        lcd_data_buf(line_buf, sizeof(line_buf));
    }
}
