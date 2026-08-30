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

#ifdef BOARD_HAS_VOL_BUTTON
    {
        static const input_pin_t vol_pins[] = {
            { GPIOA_BASE,  0, 0 },   /* PA0  - stock volume up      */
            { GPIOB_BASE,  2, 0 },   /* PB2  - stock volume down    */
            { GPIOC_BASE, 13, 0 },   /* PC13 - stock ping-pong      */
        };
        for (unsigned i = 0; i < 3; i++) {
            const uint32_t port = vol_pins[i].port;
            const uint32_t pin  = vol_pins[i].pin;
            gpio_mode(port, pin, GPIO_MODE_IN);
            uint32_t pu = GPIO_PUPDR(port);
            pu &= ~(3u << (pin * 2));
            pu |=  (1u << (pin * 2));
            GPIO_PUPDR(port) = pu;
        }
    }
#endif
}

#ifdef BOARD_HAS_VOL_BUTTON
/* Same 0..3 range as stock DAT_2000044c. Start loud so a first boot
 * matches the mixer as it was before the button existed. */
static unsigned vol_level = 3;
static unsigned vol_armed = 1;
static int      vol_dir   = 1;

static int vol_button_down(void)
{
    return !(GPIO_IDR(GPIOA_BASE) & 1u)
        || !(GPIO_IDR(GPIOB_BASE) & (1u << 2))
        || !(GPIO_IDR(GPIOC_BASE) & (1u << 13));
}

static void vol_poll(void)
{
    const int down = vol_button_down();
    if (!down) {
        vol_armed = 1;
        return;
    }
    if (!vol_armed) return;
    vol_armed = 0;
    /* Stock PC13 ping-pongs rather than wrapping loud -> mute. */
    if (vol_dir > 0) {
        if (vol_level < 3) vol_level++;
        else { vol_dir = -1; vol_level--; }
    } else {
        if (vol_level > 0) vol_level--;
        else { vol_dir = 1; vol_level++; }
    }
}
#endif

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

unsigned input_volume(void)
{
#ifdef BOARD_HAS_VOL_BUTTON
    return vol_level;
#else
    return 3;
#endif
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
#ifdef BOARD_HAS_VOL_BUTTON
    vol_poll();
#endif
    g_hostbox.scan_a = GPIO_IDR(GPIOA_BASE);
    g_hostbox.scan_b = GPIO_IDR(GPIOB_BASE);
    g_hostbox.scan_c = GPIO_IDR(GPIOC_BASE);
}
