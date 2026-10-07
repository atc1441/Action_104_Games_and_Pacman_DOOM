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
#define RCC_EN1_GPIOB   (1u << 1)    /* 0x002 - GPIOB (boot ROM USART2)    */
#define RCC_EN2_LCDMISC (1u << 4)    /* 0x010 - also set around USART2     */
#define RCC_EN0_DMA     0x00000003u  /* bits 0-1; DMA clock (with audio)   */
#define RCC_EN2_DMA     (1u << 9)    /* 0x200 - DMA (observed live)        */
#define RCC_APB1ENR     REG32(RCC_BASE + 0x34)
#define RCC_USART2EN    (1u << 17)   /* boot ROM FUN_00002c94              */

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
 *
 * The rest of the layout is Aisinochip's GPIO_TypeDef (ACM32G103/H5 SDK
 * names in the comments), as used by the stock HAL_GPIO_Init:
 *
 *   - +0x18 / +0x1C select the alternate function, 4 bits per pin
 *     (AF0 pins 0-7, AF1 pins 8-15).
 *   - +0x20 is drive strength, 2 bits per pin for all 16 pins. The G103
 *     and H5 use 4 bits per pin over DS0/DS1; this chip does not, and
 *     +0x24 is unused.
 *   - +0x28 is the Schmitt-trigger input enable, one bit per pin.
 *
 * Bonded pins: GPIOA 0xFF3F (no PA6/PA7), GPIOB 0x00FF, GPIOC 0xE1FF.
 */
#define GPIO_MODER(p)    REG32((p) + 0x00)   /* [V] MD, 2 bits/pin          */
#define GPIO_OTYPER(p)   REG32((p) + 0x04)   /* [V] OTYP, 1 = open drain    */
#define GPIO_PUPDR(p)    REG32((p) + 0x08)   /* [V] PUPD, 01 up, 10 down    */
#define GPIO_IDR(p)      REG32((p) + 0x0C)   /* [V] IDATA                   */
#define GPIO_ODR(p)      REG32((p) + 0x10)   /* [A] ODATA                   */
#define GPIO_BSRR(p)     REG32((p) + 0x14)   /* [V] BSC, set lo / reset hi  */
#define GPIO_AF0(p)      REG32((p) + 0x18)   /* [V] 4 bits/pin, pins 0..7   */
#define GPIO_AF1(p)      REG32((p) + 0x1C)   /* [V] 4 bits/pin, pins 8..15  */
#define GPIO_DS(p)       REG32((p) + 0x20)   /* [V] drive, 2 bits/pin 0..15 */
#define GPIO_SMIT(p)     REG32((p) + 0x28)   /* [V] Schmitt input enable    */

#define GPIO_MODE_IN      0u
#define GPIO_MODE_OUT     1u
#define GPIO_MODE_AF      2u
#define GPIO_MODE_ANALOG  3u

#define GPIO_PULL_NONE    0u
#define GPIO_PULL_UP      1u
#define GPIO_PULL_DOWN    2u

#define GPIO_SET(p, n)  (GPIO_BSRR(p) = (1u << (n)))
#define GPIO_CLR(p, n)  (GPIO_BSRR(p) = (1u << ((n) + 16)))

static inline void gpio_mode(uint32_t port, uint32_t pin, uint32_t mode)
{
    uint32_t m = GPIO_MODER(port);
    m &= ~(3u << (pin * 2));
    m |=  (mode & 3u) << (pin * 2);
    GPIO_MODER(port) = m;
}

/*
 * One pin, configured the way the stock HAL_GPIO_Init does it: AF number
 * (AF mode only), drive strength and push-pull (output and AF modes),
 * pull, Schmitt enable, then the mode last. Only the given pin's fields
 * are touched. That matters on GPIOA, where PA13/PA14 are SWD: overwriting
 * a whole register takes the debug port down.
 */
static inline void gpio_config(uint32_t port, uint32_t pin, uint32_t mode,
                               uint32_t pull, uint32_t drive, uint32_t af)
{
    const uint32_t s2 = pin * 2;

    if (mode == GPIO_MODE_AF) {
        volatile uint32_t *afr = pin < 8 ? &GPIO_AF0(port) : &GPIO_AF1(port);
        const uint32_t s4 = (pin & 7u) * 4;
        *afr = (*afr & ~(0xFu << s4)) | ((af & 0xFu) << s4);
    }
    if (mode == GPIO_MODE_OUT || mode == GPIO_MODE_AF) {
        GPIO_DS(port) = (GPIO_DS(port) & ~(3u << s2)) | ((drive & 3u) << s2);
        GPIO_OTYPER(port) &= ~(1u << pin);
    }
    GPIO_PUPDR(port) = (GPIO_PUPDR(port) & ~(3u << s2)) | ((pull & 3u) << s2);
    GPIO_SMIT(port) |= 1u << pin;
    gpio_mode(port, pin, mode);
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
 * TIM1 - audio PWM                                           [V]
 *
 * Sound is 8-bit PWM from TIM1, fed by DMA. Driver in sdk/audio.c; mixer
 * in port/i_sound_console.c. Confirmed on hardware (menu blips and SFX).
 *
 * 0x40012C00 is TIM1 in the Aisinochip ACM32G103 and ACM32H5 SDKs, and
 * every value observed while the stock firmware was playing decodes as
 * a PWM setup there (see docs/SOC_IDENTIFICATION.md). Register names and
 * bit positions below are the SDK's.
 *
 * Stock: two ping-pong buffers in SRAM; DMA channel 0 streams them into
 * TIM1_CCR1 via an 8-word self-looping descriptor, one sample per timer
 * period. Sample values come from 8-bit PCM divided by a volume divisor.
 * Volume is software; the 104 Games case has one button that ping-pongs
 * levels 0..3.
 *
 * Register values observed while the stock firmware was playing:
 *   CR1  = 0x00000081   DIER = 0x00000200   SR   = 0x00000003
 *   CCMR1= 0x00000068   CCER = 0x00000001   CNT  = live counter
 *   PSC  = 0x00000010   ARR  = 0x000000FF   BDTR = 0x00008000
 *
 * The start sequence, from app_board_init:
 *   CCER &= ~CC1E;  DMA enable &= ~1;  CCR1 = 0;  EGR = <val>;
 *   DMA enable |= 1;  BDTR |= MOE;  CR1 |= CEN;  CCER |= CC1E
 * Teardown is the reverse. RCC+0x38 bit 6 is toggled off and on around
 * it, which is the block's reset.
 *
 * Sample rate = f_TIM / ((PSC + 1) * (ARR + 1)) = f_TIM / 4352.
 * ------------------------------------------------------------------ */
#define TIM1_BASE   0x40012C00u
#define TIM1_CR1    REG32(TIM1_BASE + 0x00)
#define TIM1_DIER   REG32(TIM1_BASE + 0x0C)
#define TIM1_SR     REG32(TIM1_BASE + 0x10)
#define TIM1_EGR    REG32(TIM1_BASE + 0x14)
#define TIM1_CCMR1  REG32(TIM1_BASE + 0x18)
#define TIM1_CCER   REG32(TIM1_BASE + 0x20)
#define TIM1_CNT    REG32(TIM1_BASE + 0x24)   /* counts while playing  */
#define TIM1_PSC    REG32(TIM1_BASE + 0x28)
#define TIM1_ARR    REG32(TIM1_BASE + 0x2C)
#define TIM1_CCR1   REG32(TIM1_BASE + 0x34)   /* DMA writes here       */
#define TIM1_BDTR   REG32(TIM1_BASE + 0x44)

#define TIM_CR1_CEN         (1u << 0)    /* counter enable                */
#define TIM_CR1_ARPE        (1u << 7)    /* ARR preload                   */
#define TIM_SR_UIF          (1u << 0)    /* update flag                   */
#define TIM_SR_CC1IF        (1u << 1)    /* capture/compare 1 flag        */
#define TIM_DIER_CC1DE      (1u << 9)    /* DMA request on CC1            */
#define TIM_CCMR1_OC1PE     (1u << 3)    /* CCR1 preload                  */
#define TIM_CCMR1_OC1M_PWM1 (6u << 4)    /* OC1M = PWM mode 1             */
#define TIM_CCER_CC1E       (1u << 0)    /* channel 1 output enable       */
#define TIM_BDTR_MOE        (1u << 15)   /* main output enable            */

/* TIM1 channel 1 output to the speaker: PA15, AF2. */
#define TIM1_CH1_PORT  GPIOA_BASE
#define TIM1_CH1_PIN   15u
#define TIM1_CH1_AF    2u

/* RCC+0x38, called RCC_EN2 above - toggled off and on to reset TIM1.
 * (IDA labels the same register RCC_EN3.) */
#define RCC_EN2_TIM1   (1u << 6)

/* ------------------------------------------------------------------ *
 * DMA                                                        [V]
 *
 * Channels are 0x40 apart starting at 0x40031100. Two are in use:
 *   channel 0  ->  TIM1_CCR1 (self-loop, CTRL high 0x8540, CFG 0x000A0083)
 *   channel 1  ->  LCD SPI data 0x40030000 (one-shot, lcd.c spi_write_dma)
 *
 * Channel 1 live values from stock FUN_08027464 (NES lines were 512 bytes;
 * DOOM rows are 480, fill rows 640). Do not copy the audio descriptor:
 *   NEXT = 0
 *   CTRL = 0x04120000 | byte_length
 *   CFG  = 0x00010003
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

#define DMA_CH1_CTRL_HI  0x04120000u   /* LCD SPI 8-bit one-shot, FUN_08027464 */
#define DMA_CH1_CFG      0x00010003u   /* ch1 enable; not the audio 0x000A0083 */

/* ------------------------------------------------------------------ *
 * UART                                                       [V]
 *
 * Boot ROM logs over USART2 at 115200 8N1 (FUN_00002e5c / FUN_00002d64).
 * The case exposes that one UART. USART1 (PA9/PA10, AF 1) exists but is
 * not brought out. The stock *application* printf putchar is `bx lr`, so
 * FlyThings strings never actually left the chip; only the boot ROM did.
 *
 * Layout from boot ROM + HAL_UART_Transmit (app FUN_08005708):
 *   +0x00  TDR (write a byte)
 *   +0x04  SR  bit 5 = TX full (wait while set), bit 9 = TX busy
 *   +0x08  baud (integer in bits 21:6, 6-bit fraction in 5:0)
 *   +0x14  CR  bit 0 = enable, 0x300 = TX+RX (FUN_00002c60)
 *   +0x1C  0xE0 = 8N1 | 0x20
 *
 * Pins: GPIOB 0/1, AF 1 (boot ROM FUN_00002c94). PB0 is also KEY_A.
 * ------------------------------------------------------------------ */
#define USART1_BASE  0x40013800u
#define USART2_BASE  0x40004400u
#define USART_TDR(b)   REG32((b) + 0x00)
#define USART_SR(b)    REG32((b) + 0x04)
#define USART_BRR(b)   REG32((b) + 0x08)
#define USART_CR(b)    REG32((b) + 0x14)
#define USART_CR1C(b)  REG32((b) + 0x1C)
#define USART_SR_TXFULL  (1u << 5)
#define USART_SR_TXBUSY  (1u << 9)

/* ------------------------------------------------------------------ *
 * Cortex-M system registers
 * ------------------------------------------------------------------ */
#define SCB_VTOR     REG32(0xE000ED08u)
#define SCB_AIRCR    REG32(0xE000ED0Cu)   /* 0x05FA0004 = system reset */
#define DWT_CYCCNT   REG32(0xE0001004u)
#define DWT_CTRL     REG32(0xE0001000u)
#define DEMCR        REG32(0xE000EDFCu)

#endif /* SOC_H */
