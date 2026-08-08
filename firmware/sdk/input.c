/*
 * input.c - button scanning
 *
 * The pin-to-key table comes from boards/<board>/board.h. How it was
 * obtained for the 104-games board is documented there; the short version
 * is that it was read out of the stock key scan and then confirmed by
 * pressing every button on the device.
 *
 * All buttons are active low.
 */

#include "input.h"
#include "soc.h"
#include "board.h"
#include "hostbox.h"

typedef struct {
    input_pin_t pin;
    uint16_t    key;
} board_key_t;

static const board_key_t board_keys[] = { BOARD_KEY_TABLE };

#define NUM_KEYS (sizeof(board_keys) / sizeof(board_keys[0]))

const unsigned input_num_pins = NUM_KEYS;

void input_init(void)
{
    /* Clocks for GPIOA/B/C. The stock application sets bit 0 of RCC_EN1
     * for GPIOA; the bits for B and C are not identified yet, so set the
     * low three to be safe. */
    RCC_EN1 |= 0x7u;

    for (unsigned i = 0; i < NUM_KEYS; i++) {
        const uint32_t port = board_keys[i].pin.port;
        const uint32_t pin  = board_keys[i].pin.pin;

        gpio_mode(port, pin, GPIO_MODE_IN);

        /* Enable the pull-up. Without it the inputs float and a button
         * pressed against ground changes nothing - several measurement
         * attempts died on exactly that. */
        uint32_t pu = GPIO_PUPDR(port);
        pu &= ~(3u << (pin * 2));
        pu |=  (1u << (pin * 2));      /* 01 = pull-up */
        GPIO_PUPDR(port) = pu;
    }
}

uint32_t input_read(void)
{
    uint32_t keys = 0;

    for (unsigned i = 0; i < NUM_KEYS; i++) {
        const board_key_t *k = &board_keys[i];
        if (!(GPIO_IDR(k->pin.port) & (1u << k->pin.pin))) {
            keys |= k->key;
        }
    }
    return keys;
}

uint32_t input_raw(void)
{
    uint32_t mask = 0;

    for (unsigned i = 0; i < NUM_KEYS; i++) {
        if (!(GPIO_IDR(board_keys[i].pin.port) & (1u << board_keys[i].pin.pin))) {
            mask |= (1u << i);
        }
    }
    return mask;
}

/* ------------------------------------------------------------------ *
 * Scan mode for working out a new board's button map
 *
 * Why this runs in the firmware rather than in the host tool: writing to
 * the GPIO registers over SWD wedges the debug bus on this SoC - halted
 * core or not - and afterwards the CPU is unreachable until a power
 * cycle. SRAM access is fine. So the firmware mirrors the input registers
 * to a fixed address and the host only ever reads.
 *
 * Note what this deliberately does NOT do: reconfigure pins. A first
 * version set every unused pin to input with a pull-up, which crashed the
 * console at startup - something on one of those pins is needed as an
 * output. The registers are published exactly as the bootloader and
 * input_init() leave them.
 * ------------------------------------------------------------------ */
void input_scan_init(void)
{
    g_hostbox.scan_magic = HOSTBOX_SCAN_MAGIC;
}

void input_publish(void)
{
    g_hostbox.scan_a = GPIO_IDR(GPIOA_BASE);
    g_hostbox.scan_b = GPIO_IDR(GPIOB_BASE);
    g_hostbox.scan_c = GPIO_IDR(GPIOC_BASE);
}
