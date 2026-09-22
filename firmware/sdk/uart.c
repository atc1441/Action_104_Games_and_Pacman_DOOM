/*
 * uart.c - console UART, rebuilt from the boot ROM
 *
 * Boot ROM FUN_00002e5c / FUN_00002d64 talks USART2 at 115200 8N1 on
 * PB0/PB1. The stock *application* printf putchar is `bx lr`, so those
 * FlyThings strings never left the chip.
 *
 * The Pico 2 CDC UART did not see USART2 traffic with that pinout live,
 * so this driver also brings up USART1 on PA9/PA10 (boot ROM's other
 * instance) and writes both. Whichever header the case exposes will get
 * the bytes; PA9/PA10 are not game buttons.
 */

#include "uart.h"
#include "soc.h"
#include "clock.h"

#define UART_BAUD  115200u
#define UART_AF    1u

static void uart_set_baud(uint32_t base, uint32_t baud)
{
    uint32_t pclk = g_cpu_hz;
    uint32_t r20  = REG32(RCC_BASE + 0x20);

    if ((r20 & 0x0Fu) != 0)
        pclk >>= 1;
    if (base == USART2_BASE) {
        if ((r20 & 0x700u) != 0)
            pclk >>= 1;
    } else {
        if ((r20 & 0x3800u) != 0)
            pclk >>= 1;
    }

    uint32_t over16 = baud * 16u;
    uint32_t ipart  = pclk / over16;
    uint32_t rem    = pclk - ipart * over16;
    uint32_t frac   = (uint32_t)(((uint64_t)rem * 1000000ull) / over16);
    frac = (uint32_t)(((uint64_t)frac * 64ull + 500000ull) / 1000000ull);

    uint32_t brr = USART_BRR(base) & 0xFFC0003Fu;
    uint32_t scaled = ipart * 0x40u;
    if (frac > 0x3Fu) {
        USART_BRR(base) = ((scaled + 0x40u) & 0x3FFFC0u) | brr;
        USART_BRR(base) &= ~0x3Fu;
    } else {
        USART_BRR(base) = brr | (scaled & 0x3FFFC0u);
        USART_BRR(base) = (USART_BRR(base) & ~0x3Fu) | (frac & 0x3Fu);
    }
}

static void uart_block_init(uint32_t base)
{
    uart_set_baud(base, UART_BAUD);
    USART_CR1C(base) = 0xC0u | 0x20u;
    USART_CR(base)   = 0x300u;
    USART_CR(base)  |= 1u;
    if (base == USART1_BASE)
        REG32(base + 0x0C) = 0x2010u;
}

static void uart_gpio_usart2(void)
{
    RCC_APB1ENR |= RCC_USART2EN;
    RCC_EN1     |= RCC_EN1_GPIOB;
    RCC_EN2     |= RCC_EN2_LCDMISC;

    uint32_t m = GPIO_MODER(GPIOB_BASE);
    m &= ~((3u << 0) | (3u << 2));
    m |=  (GPIO_MODE_AF << 0) | (GPIO_MODE_AF << 2);
    GPIO_MODER(GPIOB_BASE) = m;

    uint32_t pu = GPIO_PUPDR(GPIOB_BASE);
    pu &= ~((3u << 0) | (3u << 2));
    pu |=  (1u << 0) | (1u << 2);
    GPIO_PUPDR(GPIOB_BASE) = pu;

    uint32_t afr18 = REG32(GPIOB_BASE + 0x18);
    afr18 &= ~0xFFu;
    afr18 |= (UART_AF << 0) | (UART_AF << 4);
    REG32(GPIOB_BASE + 0x18) = afr18;

    uint32_t afr20 = GPIO_AFRL(GPIOB_BASE);
    afr20 &= ~0xFFu;
    afr20 |= (UART_AF << 0) | (UART_AF << 4);
    GPIO_AFRL(GPIOB_BASE) = afr20;
}

static void uart_gpio_usart1(void)
{
    /* Boot ROM FUN_00002c94: GPIOA clock, EN2 bits 4 and 9, PA9+PA10 AF1. */
    RCC_EN1 |= RCC_EN1_GPIOA;
    RCC_EN2 |= 0x210u;
    RCC_APB1ENR |= (1u << 14);   /* STM32-style USART1EN; harmless if unused */

    uint32_t m = GPIO_MODER(GPIOA_BASE);
    m &= ~((3u << 18) | (3u << 20));
    m |=  (GPIO_MODE_AF << 18) | (GPIO_MODE_AF << 20);
    GPIO_MODER(GPIOA_BASE) = m;

    uint32_t pu = GPIO_PUPDR(GPIOA_BASE);
    pu &= ~((3u << 18) | (3u << 20));
    pu |=  (1u << 18) | (1u << 20);
    GPIO_PUPDR(GPIOA_BASE) = pu;

    uint32_t afr1c = REG32(GPIOA_BASE + 0x1C);
    afr1c &= ~(0xFFu << 4);
    afr1c |= (UART_AF << 4) | (UART_AF << 8);
    REG32(GPIOA_BASE + 0x1C) = afr1c;

    uint32_t afrh = GPIO_AFRH(GPIOA_BASE);
    afrh &= ~(0xFFu << 4);
    afrh |= (UART_AF << 4) | (UART_AF << 8);
    GPIO_AFRH(GPIOA_BASE) = afrh;
}

static void uart_putc_block(uint32_t base, uint8_t c)
{
    USART_CR(base) |= 0x100u;
    USART_TDR(base) = c;
    while (USART_SR(base) & USART_SR_TXFULL) { }
    while (USART_SR(base) & USART_SR_TXBUSY) { }
}

void uart_init(void)
{
    uart_gpio_usart2();
    uart_block_init(USART2_BASE);
    uart_gpio_usart1();
    uart_block_init(USART1_BASE);
}

void uart_putc(uint8_t c)
{
    uart_putc_block(USART2_BASE, c);
    uart_putc_block(USART1_BASE, c);
}

void uart_write(const void *buf, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n--) {
        uint8_t c = *p++;
        if (c == '\n')
            uart_putc('\r');
        uart_putc(c);
    }
}

void uart_puts(const char *s)
{
    if (!s)
        return;
    while (*s)
        uart_write(s++, 1);
}

int _write(int fd, const char *buf, int len)
{
    (void)fd;
    if (len <= 0 || !buf)
        return 0;
    uart_write(buf, (uint32_t)len);
    return len;
}

int _isatty(int fd)
{
    (void)fd;
    return 1;
}
