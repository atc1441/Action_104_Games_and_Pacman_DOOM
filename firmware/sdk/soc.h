/*
 * soc.h - register map of the console SoC
 *
 * Reconstructed from bootloader and application disassembly plus live
 * register dumps taken over SWD. There is no datasheet. Every definition
 * carries its provenance:
 *
 *   [V] verified - proven from the stock firmware's code, or measured live
 *   [A] assumed  - inferred from the STM32L4-like layout, NOT proven
 *
 * SoC: STAR-MC1 (Arm China, ARMv8-M Mainline, Cortex-M33 compatible)
 *      CPUID 0x631F1320, 8 KB I-cache + 8 KB D-cache, no TrustZone.
 *
 * The peripheral base addresses look exactly like an STM32L4. The register
 * layouts do not. Anyone who assumes ST silicon here and reaches for an
 * STM32 SDK will spend a long time chasing ghosts - see the GPIO note
 * below for what that costs.
 */

#ifndef SOC_H
#define SOC_H

#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* ------------------------------------------------------------------ *
 * Memory map                                                 [V]
 * ------------------------------------------------------------------ */
#define BOOTROM_BASE    0x00000000u   /* 32 KB, aliased every 32 KB         */
#define XIP_FLASH_BASE  0x08000000u   /* 4 MB external SPI flash, mapped    */
#define XIP_FLASH_SIZE  0x00400000u
#define APP_BASE        0x08004000u   /* where the bootloader jumps         */
#define SRAM_BASE       0x20000000u
#define SRAM_SIZE       0x00046000u   /* 280 KB - measured on the device.
                                       * A core read above this triggers a
                                       * system reset. Over SWD the same
                                       * range reads back 0xAA without
                                       * complaint, which is simply how the
                                       * bus answers for absent memory -
                                       * not a fill pattern.               */

/* ------------------------------------------------------------------ *
 * RCC - clock control                                        [V]
 * The three enable registers are proven: the stock firmware sets exactly
 * these bits before its LCD init (app 0x0802FC52..0x0802FC7A).
 * ------------------------------------------------------------------ */
#define RCC_BASE   0x40021000u
#define RCC_EN0    REG32(RCC_BASE + 0x28)
#define RCC_EN1    REG32(RCC_BASE + 0x2C)
#define RCC_EN2    REG32(RCC_BASE + 0x38)

#define RCC_EN0_LCDSPI  (1u << 10)   /* 0x400 - SPI controller 0x40030000  */
#define RCC_EN1_GPIOA   (1u << 0)    /* 0x001 - GPIOA                      */
#define RCC_EN2_LCDMISC (1u << 4)    /* 0x010 - purpose still unknown      */

/* ------------------------------------------------------------------ *
 * GPIO                                                       [V]/[A]
 * ------------------------------------------------------------------ */
#define GPIOA_BASE  0x48000000u   /* [V] */
#define GPIOB_BASE  0x48000400u   /* [V] */
#define GPIOC_BASE  0x48000800u   /* [V] */

/*
 * The layout is NOT the STM32 one, even though the base addresses match.
 * Three deviations are proven:
 *
 *   - the input register is at +0x0C, not +0x10
 *     (the stock key scan reads it there, VMA 0x20036000 ff.)
 *   - BSRR is at +0x14, not +0x18
 *     (the stock LCD routine uses it that way, app 0x0802FC3E ff.)
 *   - the pull configuration is at +0x08, not +0x0C
 *     (gpio_init of the stock app, 0x08004C5A: two bits per pin. In
 *      operation GPIOA +0x08 reads 0xA5540555, i.e. 01 = pull-up on the
 *      populated pins.)
 *
 * The third one cost real time: OSPEEDR was assumed at +0x08, so input
 * init never enabled any pull-up. The inputs floated and pressing a
 * button against ground changed nothing at all.
 */
#define GPIO_MODER(p)    REG32((p) + 0x00)   /* [V] 2 bits/pin             */
#define GPIO_OTYPER(p)   REG32((p) + 0x04)   /* [A]                        */
#define GPIO_PUPDR(p)    REG32((p) + 0x08)   /* [V] 2 bits/pin, 01=pull-up */
#define GPIO_IDR(p)      REG32((p) + 0x0C)   /* [V] input state            */
#define GPIO_ODR(p)      REG32((p) + 0x10)   /* [A] presumably output      */
#define GPIO_BSRR(p)     REG32((p) + 0x14)   /* [V] set lo16 / reset hi16  */
#define GPIO_AFRL(p)     REG32((p) + 0x20)   /* [A] 4 bits/pin, pins 0..7  */
#define GPIO_AFRH(p)     REG32((p) + 0x24)   /* [A] 4 bits/pin, pins 8..15 */

#define GPIO_MODE_IN      0u
#define GPIO_MODE_OUT     1u
#define GPIO_MODE_AF      2u
#define GPIO_MODE_ANALOG  3u

#define GPIO_SET(p, n)  (GPIO_BSRR(p) = (1u << (n)))
#define GPIO_CLR(p, n)  (GPIO_BSRR(p) = (1u << ((n) + 16)))

static inline void gpio_mode(uint32_t port, uint32_t pin, uint32_t mode)
{
    uint32_t m = GPIO_MODER(port);
    m &= ~(3u << (pin * 2));
    m |=  (mode & 3u) << (pin * 2);
    GPIO_MODER(port) = m;
}

/* ------------------------------------------------------------------ *
 * SPI controllers                                            [V]
 * Two instances with an identical register layout:
 *   0x40030000 - LCD
 *   0x52005000 - external flash. DO NOT TOUCH while running from flash:
 *                XIP goes through it, i.e. the very code the CPU is
 *                currently executing.
 * Offsets derived from the stock firmware's byte-send sequence
 * (app 0x0803000A..0x08030024).
 * ------------------------------------------------------------------ */
#define SPI_LCD_BASE    0x40030000u
#define SPI_FLASH_BASE  0x52005000u   /* XIP; write +0x04/+0x10 only from RAM */

/* BootROM FUN_00002800 writes the low 16 bits of +0x04 (DIV2FAIL / DIV4FAIL).
 * Stock app_board_init only calls its RAM stub when that field is 2 and
 * +0x10 bits 8:9 are clear. */
#define SPI_FLASH_DIV    REG32(SPI_FLASH_BASE + 0x04)
#define SPI_FLASH_RXCTL  REG32(SPI_FLASH_BASE + 0x10)

#define SPI_DR(b)    REG32((b) + 0x00)   /* data register, byte at a time  */
#define SPI_CR0C(b)  REG32((b) + 0x0C)   /* set bit0 before every byte     */
#define SPI_SR(b)    REG32((b) + 0x18)   /* status                         */
#define SPI_CR20(b)  REG32((b) + 0x20)   /* set to 1 before every byte     */
#define SPI_CR24(b)  REG32((b) + 0x24)   /* bit0 = start transfer          */

#define SPI_SR_BUSY  (1u << 0)    /* the stock code tests it via lsls #31 */
#define SPI_SR_TXE   (1u << 14)   /* the stock code tests it via lsls #17 */

/* ------------------------------------------------------------------ *
 * Audio                                                      [V]
 *
 * A PCM/DAC block fed by DMA. Driver in sdk/audio.c; mixer in
 * port/i_sound_console.c. Confirmed on hardware (menu blips and SFX).
 *
 * Stock: two ping-pong buffers in SRAM; DMA channel 0 streams them into
 * AUDIO_DATA via an 8-word self-looping descriptor. Sample values come
 * from 8-bit PCM divided by a volume divisor. Volume is software; the
 * 104 Games case has one button that ping-pongs levels 0..3.
 *
 * Register values observed while the stock firmware was playing:
 *   +0x00 = 0x00000081   +0x0C = 0x00000200   +0x10 = 0x00000003
 *   +0x18 = 0x00000068   +0x20 = 0x00000001   +0x24 = live counter
 *   +0x28 = 0x00000010   +0x2C = 0x000000FF   +0x44 = 0x00008000
 *
 * The start sequence, from app_board_init:
 *   +0x20 &= ~1;  DMA enable &= ~1;  +0x34 = 0;  +0x14 = <val>;
 *   DMA enable |= 1;  +0x44 |= 0x8000;  +0x00 |= 1;  +0x20 |= 1
 * Teardown is the reverse. RCC+0x38 bit 6 is toggled off and on around
 * it, which is the block's reset.
 * ------------------------------------------------------------------ */
#define AUDIO_BASE   0x40012C00u
#define AUDIO_DATA   REG32(AUDIO_BASE + 0x34)   /* DMA writes here */
#define AUDIO_EN     REG32(AUDIO_BASE + 0x20)   /* bit0 */
#define AUDIO_CR0    REG32(AUDIO_BASE + 0x00)   /* bit0 */
#define AUDIO_CR44   REG32(AUDIO_BASE + 0x44)   /* bit15 */

/* RCC+0x38, called RCC_EN2 above - toggled off and on to reset the
 * audio block. (IDA labels the same register RCC_EN3.) */
#define RCC_EN2_AUDIO  (1u << 6)

/* ------------------------------------------------------------------ *
 * DMA                                                        [V]
 *
 * Channels are 0x40 apart starting at 0x40031100. Two are in use by the
 * stock firmware:
 *   channel 0  ->  AUDIO_DATA
 *   channel 1  ->  the LCD SPI data register (0x40030000)
 *
 * That second one is worth remembering: the display could be driven by
 * DMA instead of the byte-pushing loop in lcd.c, which is where the frame
 * rate is currently going.
 *
 * Live values for the audio channel: SRC advancing through the buffer,
 * DST = 0x40012C34, +0x0C = 0x854002CA (the low half counts down, so it
 * is the remaining transfer count), +0x10 = 0x000A0083 with bit 0 as the
 * channel enable.
 * ------------------------------------------------------------------ */
#define DMA_BASE       0x40031000u
#define DMA_GLOBAL_EN  REG32(DMA_BASE + 0x30)   /* controller master enable */
#define DMA_GLOBAL_1C  REG32(DMA_BASE + 0x1C)   /* also 1 while streaming  */
#define DMA_IRQ_CLR    REG32(DMA_BASE + 0x08)   /* write 1 to acknowledge */
#define DMA_STATUS     REG32(DMA_BASE + 0x14)   /* bit0 = channel 0 done  */
#define DMA_CH(n)      (DMA_BASE + 0x100 + (n) * 0x40)
#define DMA_CH_SRC(n)  REG32(DMA_CH(n) + 0x00)
#define DMA_CH_DST(n)  REG32(DMA_CH(n) + 0x04)
#define DMA_CH_NEXT(n) REG32(DMA_CH(n) + 0x08)  /* -> word holding next SRC */
#define DMA_CH_CTRL(n) REG32(DMA_CH(n) + 0x0C)
#define DMA_CH_CFG(n)  REG32(DMA_CH(n) + 0x10)  /* bit0 = enable */

/* ------------------------------------------------------------------ *
 * UART                                                       [V]
 * The bootloader logs over USART2, the application over USART1.
 * Register layout not reconstructed.
 * ------------------------------------------------------------------ */
#define USART1_BASE  0x40013800u
#define USART2_BASE  0x40004400u

/* ------------------------------------------------------------------ *
 * Cortex-M system registers
 * ------------------------------------------------------------------ */
#define SCB_VTOR     REG32(0xE000ED08u)
#define SCB_AIRCR    REG32(0xE000ED0Cu)   /* 0x05FA0004 = system reset */
#define DWT_CYCCNT   REG32(0xE0001004u)
#define DWT_CTRL     REG32(0xE0001000u)
#define DEMCR        REG32(0xE000EDFCu)

#endif /* SOC_H */
