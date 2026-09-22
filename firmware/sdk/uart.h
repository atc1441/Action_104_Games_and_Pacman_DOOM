#ifndef UART_H
#define UART_H

#include <stdint.h>

/* USART2, 115200 8N1, boot ROM pinout. Safe to call more than once. */
void uart_init(void);
void uart_putc(uint8_t c);
void uart_write(const void *buf, uint32_t n);
void uart_puts(const char *s);

#endif /* UART_H */
