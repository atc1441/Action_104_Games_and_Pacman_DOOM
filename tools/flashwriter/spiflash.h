#ifndef SPIFLASH_H
#define SPIFLASH_H

#include <stdint.h>

/* Muss aus dem RAM laufen — siehe Kommentar in spiflash.c */
void     spiflash_enter_manual(void);
void     spiflash_restore_xip(void);
uint32_t spiflash_jedec_id(void);
int      spiflash_erase_sector(uint32_t addr);                              /* 4 KB      */
int      spiflash_program_page(uint32_t addr, const uint8_t *d, uint32_t n); /* max 256 B */
int      spiflash_read(uint32_t addr, uint8_t *d, uint32_t n);

#endif
