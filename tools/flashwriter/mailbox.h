#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdint.h>

/*
 * Mailbox between the host (J-Link/pylink) and the RAM-resident writer.
 * The writer runs continuously and polls cmd; the host writes the
 * parameters, sets cmd, and waits for the writer to clear cmd again.
 * SRAM access over SWD works while the core is running.
 */
#define MBOX_ADDR    0x2001E000u
#define BUF_ADDR     0x20020000u
#define BUF_SIZE     0x10000u        /* 64 KB */

#define MBOX_MAGIC   0xF1A54EE5u     /* writer is up and ready */

enum {
    CMD_IDLE     = 0,
    CMD_JEDEC    = 1,
    CMD_ERASE_4K = 2,   /* addr                                 */
    CMD_PROGRAM  = 3,   /* addr, len, data in BUF_ADDR          */
    CMD_READ     = 4,   /* addr, len -> data into BUF_ADDR      */
    CMD_EXIT     = 5,   /* restore XIP mode and stop            */
};

typedef struct {
    volatile uint32_t cmd;
    volatile uint32_t addr;
    volatile uint32_t len;
    volatile uint32_t status;   /* 0 = ok */
    volatile uint32_t result;   /* e.g. the JEDEC id */
    volatile uint32_t magic;
} mbox_t;

#endif
