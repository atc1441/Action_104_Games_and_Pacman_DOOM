/*
 * uart.c - console UART, rebuilt from the boot ROM
 *
 * The boot ROM talks USART2 at 115200 8N1 on PB0/PB1; its other instance
 * is USART1 on PA9/PA10. Which one the case pads carry is not
 * established, so this driver brings up both and writes every byte to
 * both. PA9/PA10 are not game buttons.
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
    RCC_EN3 |= RCC_EN3_USART2;
    RCC_EN1 |= RCC_EN1_GPIOB;
    RCC_EN2 |= RCC_EN2_EXTI;

    /* PB0/PB1 AF1, pull-up, drive 3, as the boot ROM sets them. */
    gpio_config(GPIOB_BASE, 0, GPIO_MODE_AF, GPIO_PULL_UP, 3u, UART_AF);
    gpio_config(GPIOB_BASE, 1, GPIO_MODE_AF, GPIO_PULL_UP, 3u, UART_AF);
}

static void uart_gpio_usart1(void)
{
    RCC_EN2 |= RCC_EN2_USART1;
    RCC_EN1 |= RCC_EN1_GPIOA;
    RCC_EN2 |= RCC_EN2_EXTI;

    /* PA9/PA10 AF1, pull-up, drive 3, as the boot ROM sets them. */
    gpio_config(GPIOA_BASE,  9, GPIO_MODE_AF, GPIO_PULL_UP, 3u, UART_AF);
    gpio_config(GPIOA_BASE, 10, GPIO_MODE_AF, GPIO_PULL_UP, 3u, UART_AF);
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
