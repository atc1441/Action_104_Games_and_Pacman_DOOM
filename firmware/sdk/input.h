#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/*
 * Logical keys.
 *
 * The order is the bit order of the mask the stock firmware builds in
 * g_key_mask (0x20030329) - which is exactly an NES joypad byte. That is
 * no accident: the stock application is an NES emulator.
 */
#define KEY_A       0x01u
#define KEY_B       0x02u
#define KEY_SELECT  0x04u      /* labelled MENU on the case */
#define KEY_START   0x08u
#define KEY_UP      0x10u
#define KEY_DOWN    0x20u
#define KEY_LEFT    0x40u
#define KEY_RIGHT   0x80u

/*
 * The two turbo buttons. The stock firmware ORs them onto A and B behind
 * a counter; here they get keys of their own, because DOOM has better
 * uses for two extra buttons than duplicating fire.
 */
#define KEY_TURBO_A 0x100u
#define KEY_TURBO_B 0x200u

typedef struct {
    uint32_t    port;
    uint8_t     pin;
    const char *name;
} input_pin_t;

/* Which pins exist and what they do is board-specific; see
 * boards/<board>/board.h. The table itself stays private to input.c -
 * only its size is exported, for tools that iterate raw pin states. */
extern const unsigned input_num_pins;

void     input_init(void);

/* Bit mask of logical keys (KEY_*), active-low already resolved. */
uint32_t input_read(void);

/* Raw mask: bit i = pin i of input_pins[] is pressed. For measurements. */
uint32_t input_raw(void);

/* Scan mode: mirror all three GPIO input registers into the host mailbox
 * so a debugger can watch them while buttons are pressed. Deliberately
 * does not touch pin configuration - see input.c. */
void     input_scan_init(void);
void     input_publish(void);

#endif /* INPUT_H */
