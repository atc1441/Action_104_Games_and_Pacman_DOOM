#ifndef HOSTBOX_H
#define HOSTBOX_H

#include <stdint.h>

/*
 * Shared mailbox between firmware and host (J-Link / SWD).
 *
 * The linker script pins this structure into section .mailbox, which puts
 * it at a fixed 0x20000000. That matters: earlier versions used guessed
 * addresses around 0x2000FD00, which sit right inside DOOM's zone heap -
 * it starts just past .bss and grows over them. Both sides quietly
 * overwrote each other's data.
 *
 * At the bottom of RAM the space is permanently free (the vector table
 * lives in flash) and the offsets no longer move when .bss grows, so host
 * tools may hard-code them:
 *
 *   0x20000000  key_magic     "KEYB", otherwise keys is ignored
 *   0x20000004  keys          key bits, written by the host
 *   0x20000008  scan_a/b/c    GPIO input registers, written by firmware
 *   0x20000014  scan_magic    "SCAN"
 *   0x20000018  mark          progress marker, survives a warm reset
 *   0x2000001C  markv         value belonging to it (e.g. a register)
 *   0x20000020  err_magic     set when err_text is valid
 *   0x20000024  err_text      message from I_Error
 *
 * The section is NOLOAD, so the contents survive a warm reset - which is
 * the whole point when you want to read why the firmware died.
 */
#define HOSTBOX_ADDR       0x20000000u
#define HOSTBOX_KEY_MAGIC  0x4B455942u   /* "KEYB" */
#define HOSTBOX_SCAN_MAGIC 0x5343414Eu   /* "SCAN" */
#define HOSTBOX_ERR_MAGIC  0xE7707E77u

typedef struct {
    volatile uint32_t key_magic;
    volatile uint32_t keys;
    volatile uint32_t scan_a;
    volatile uint32_t scan_b;
    volatile uint32_t scan_c;
    volatile uint32_t scan_magic;
    volatile uint32_t mark;
    volatile uint32_t markv;
    volatile uint32_t err_magic;
    volatile char     err_text[0xDC];
} hostbox_t;

extern hostbox_t g_hostbox;

#endif /* HOSTBOX_H */
