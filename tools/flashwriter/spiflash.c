/*
 * spiflash.c - access to the external SPI flash via the MPI controller
 *
 * Every register sequence is taken from the stock bootloader, not guessed:
 *   mode switch        bootloader 0x3B44
 *   transmit (TX)      bootloader 0x2B60
 *   receive (RX)       bootloader 0x29F8
 *   chip select        bootloader 0x2128, GPIOC bit 0
 *
 * IMPORTANT: this code MUST run from RAM. The moment the controller
 * leaves XIP mode the flash is no longer readable as memory, so code at
 * 0x08000000 would be pulling the floor out from under itself.
 */

#include "spiflash.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define SPI_BASE   0x52005000u
#define SPI_DR     REG32(SPI_BASE + 0x00)
#define SPI_MODE   REG32(SPI_BASE + 0x08)
#define SPI_TXCTL  REG32(SPI_BASE + 0x0C)
#define SPI_RXCTL  REG32(SPI_BASE + 0x10)
#define SPI_SR     REG32(SPI_BASE + 0x18)
#define SPI_LEN    REG32(SPI_BASE + 0x20)
#define SPI_START  REG32(SPI_BASE + 0x24)

#define SR_TXFULL   (1u << 3)
#define SR_RXEMPTY  (1u << 4)
#define SR_TXDONE   (1u << 14)
#define SR_ACK_TX   (1u << 14)
#define SR_ACK_RX   (1u << 15)

#define MODE_XIP_BITS  0x60u        /* Bits 5:6 */

#define GPIOC_BASE  0x48000800u
#define GPIO_MODER  REG32(GPIOC_BASE + 0x00)
#define GPIO_BSRR   REG32(GPIOC_BASE + 0x14)
#define CS_PIN      0u              /* GPIOC Pin 0 */
#define CS_MASK     (1u << CS_PIN)

#define CS_LOW()   (GPIO_BSRR = (CS_MASK << 16))
#define CS_HIGH()  (GPIO_BSRR = (CS_MASK))

/*
 * +0x2C / +0x30 control automatic XIP access. While the stock app runs
 * they hold 0x05441849 and 0xFAEB (0xEB = Fast Read Quad I/O); while the
 * bootloader talks to the flash manually they hold 0x00040000 and 0.
 * That is the actual switch between "controller fetches by itself" and
 * "we push bytes through the FIFO" - bits 5:6 of +0x08 are NOT, they are
 * 0 in both cases. Chasing those cost a while.
 *
 * Found by breakpointing the bootloader's SPI routine and reading the
 * register state as it went.
 */
#define SPI_XIPCMD  REG32(SPI_BASE + 0x2C)
#define SPI_XIPRD   REG32(SPI_BASE + 0x30)
#define XIPCMD_MANUAL  0x00040000u

static uint32_t saved_mode;
static uint32_t saved_moder;
static uint32_t saved_xipcmd;
static uint32_t saved_xiprd;

/* --- primitives ------------------------------------------------------ */

static void spi_tx(const uint8_t *buf, uint32_t len)
{
    if (!len) return;

    SPI_SR    |= SR_ACK_TX;
    SPI_SR    |= 2u;
    SPI_TXCTL |= 2u;
    SPI_TXCTL &= ~2u;
    SPI_LEN    = len;
    SPI_TXCTL |= 1u;
    SPI_START |= 1u;

    for (uint32_t i = 0; i < len; i++) {
        while (SPI_SR & SR_TXFULL) { }
        SPI_DR = buf[i];
    }
    while (!(SPI_SR & SR_TXDONE)) { }

    SPI_SR    |= SR_ACK_TX;
    SPI_SR    |= 2u;
    SPI_TXCTL &= ~1u;
    SPI_START &= ~1u;
}

static void spi_rx(uint8_t *buf, uint32_t len)
{
    if (!len) return;

    SPI_SR    |= SR_ACK_RX;
    SPI_SR    |= 2u;
    SPI_LEN    = len;
    SPI_RXCTL |= 1u;
    SPI_START |= 1u;

    for (uint32_t i = 0; i < len; i++) {
        while (SPI_SR & SR_RXEMPTY) { }
        buf[i] = (uint8_t)SPI_DR;
    }

    SPI_SR    |= SR_ACK_RX;
    SPI_SR    |= 2u;
    SPI_RXCTL &= ~1u;
    SPI_START &= ~1u;
}

/* --- Modus ----------------------------------------------------------- */

void spiflash_enter_manual(void)
{
    saved_mode  = SPI_MODE;
    saved_moder = GPIO_MODER;

    /*
     * In normal operation chip select sits on alternate function - the
     * MPI controller drives PC0 itself (GPIOC MODER = 0x00000AAA, i.e.
     * pins 0..5 all on AF). For manual operation the pin
     * erst auf Ausgang umgestellt werden, sonst laufen alle BSRR-Writes
     * goes nowhere and the flash never sees CS asserted.
     *
     * The other pins (clock and data) stay on AF; the controller keeps
     * driving those.
     */
    uint32_t m = saved_moder;
    m &= ~(3u << (CS_PIN * 2));
    m |=  (1u << (CS_PIN * 2));     /* 01 = Ausgang */
    GPIO_MODER = m;

    CS_HIGH();

    /* Turn automatic XIP access off, or the controller interferes with
     * our manual transfers. */
    saved_xipcmd = SPI_XIPCMD;
    saved_xiprd  = SPI_XIPRD;
    SPI_XIPCMD   = XIPCMD_MANUAL;
    SPI_XIPRD    = 0;

    SPI_MODE = saved_mode & ~MODE_XIP_BITS;
}

void spiflash_restore_xip(void)
{
    CS_HIGH();
    SPI_XIPRD  = saved_xiprd;
    SPI_XIPCMD = saved_xipcmd;
    SPI_MODE   = saved_mode;
    GPIO_MODER = saved_moder;       /* hand CS back to the controller */
}

/* --- W25Q32-Befehle -------------------------------------------------- */

static void cmd_only(uint8_t op)
{
    CS_LOW();
    spi_tx(&op, 1);
    CS_HIGH();
}

static uint8_t read_status(void)
{
    uint8_t op = 0x05, sr = 0;
    CS_LOW();
    spi_tx(&op, 1);
    spi_rx(&sr, 1);
    CS_HIGH();
    return sr;
}

/* Blocks until the WIP bit clears. Returns 0 on success, 1 on timeout. */
static int wait_ready(uint32_t spins)
{
    while (spins--) {
        if (!(read_status() & 0x01u)) return 0;
    }
    return 1;
}

static void write_enable(void)
{
    cmd_only(0x06);
}

uint32_t spiflash_jedec_id(void)
{
    uint8_t op = 0x9F, id[3] = { 0, 0, 0 };
    CS_LOW();
    spi_tx(&op, 1);
    spi_rx(id, 3);
    CS_HIGH();
    return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

int spiflash_erase_sector(uint32_t addr)
{
    uint8_t c[4];

    write_enable();
    c[0] = 0x20;                       /* Sector Erase, 4 KB */
    c[1] = (uint8_t)(addr >> 16);
    c[2] = (uint8_t)(addr >> 8);
    c[3] = (uint8_t)(addr);
    CS_LOW();
    spi_tx(c, 4);
    CS_HIGH();

    return wait_ready(20000000u);
}

int spiflash_program_page(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t c[4];

    if (!len || len > 256u) return 2;

    write_enable();
    c[0] = 0x02;                       /* Page Program */
    c[1] = (uint8_t)(addr >> 16);
    c[2] = (uint8_t)(addr >> 8);
    c[3] = (uint8_t)(addr);
    CS_LOW();
    spi_tx(c, 4);
    spi_tx(data, len);
    CS_HIGH();

    return wait_ready(20000000u);
}

int spiflash_read(uint32_t addr, uint8_t *data, uint32_t len)
{
    uint8_t c[4];

    c[0] = 0x03;                       /* Read Data */
    c[1] = (uint8_t)(addr >> 16);
    c[2] = (uint8_t)(addr >> 8);
    c[3] = (uint8_t)(addr);
    CS_LOW();
    spi_tx(c, 4);
    spi_rx(data, len);
    CS_HIGH();
    return 0;
}
