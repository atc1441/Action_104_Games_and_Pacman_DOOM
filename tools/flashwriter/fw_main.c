/*
 * fw_main.c - RAM-resident flash writer
 *
 * flash.py loads this to 0x20010000 and starts it. From then on it runs
 * continuously and polls the mailbox at 0x2001E000. The host writes
 * parameters and data into SRAM over SWD, which works while the core is
 * running because SRAM access does not go through the flash controller.
 *
 * Why not start-and-halt once per page: that would mean reloading the PC
 * for each of up to 16384 pages. This way the writer stays resident and
 * works through jobs.
 */

#include <stdint.h>
#include "spiflash.h"
#include "mailbox.h"

#define MBOX  ((mbox_t *)MBOX_ADDR)
#define BUF   ((uint8_t *)BUF_ADDR)

extern uint32_t _estack;

void Reset_Handler(void);

__attribute__((section(".isr_vector"), used))
void (* const g_vectors[2])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
};

static int do_program(uint32_t addr, uint32_t len)
{
    uint32_t done = 0;

    if (len > BUF_SIZE) return 3;

    while (done < len) {
        /* page program must not cross a 256-byte boundary */
        uint32_t page_off = (addr + done) & 0xFFu;
        uint32_t chunk    = 256u - page_off;
        if (chunk > len - done) chunk = len - done;

        int rc = spiflash_program_page(addr + done, BUF + done, chunk);
        if (rc) return rc;
        done += chunk;
    }
    return 0;
}

void Reset_Handler(void)
{
    MBOX->cmd    = CMD_IDLE;
    MBOX->status = 0;
    MBOX->result = 0;

    spiflash_enter_manual();

    /* Only now announce readiness - otherwise the host could start
     * sending jobs while the controller is still in XIP mode
     * haengt. */
    MBOX->magic = MBOX_MAGIC;

    for (;;) {
        uint32_t cmd = MBOX->cmd;
        if (cmd == CMD_IDLE) continue;

        uint32_t st = 0;

        switch (cmd) {
        case CMD_JEDEC:
            MBOX->result = spiflash_jedec_id();
            break;

        case CMD_ERASE_4K:
            st = (uint32_t)spiflash_erase_sector(MBOX->addr);
            break;

        case CMD_PROGRAM:
            st = (uint32_t)do_program(MBOX->addr, MBOX->len);
            break;

        case CMD_READ:
            st = (MBOX->len > BUF_SIZE)
                     ? 3u
                     : (uint32_t)spiflash_read(MBOX->addr, BUF, MBOX->len);
            break;

        case CMD_EXIT:
            spiflash_restore_xip();
            MBOX->status = 0;
            MBOX->cmd    = CMD_IDLE;
            for (;;) { __asm volatile ("wfi"); }

        default:
            st = 0xFFu;
            break;
        }

        MBOX->status = st;
        MBOX->cmd    = CMD_IDLE;      /* Quittung — zuletzt schreiben */
    }
}
