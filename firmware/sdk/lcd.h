#ifndef LCD_H
#define LCD_H

#include <stdint.h>

/* Screen geometry and the panel init table live in
 * boards/<board>/board.h, which lcd.c includes. */

void lcd_delay_ms(uint32_t ms);
void lcd_gpio_init(void);
void lcd_init(void);
void lcd_cmd(uint8_t c);
void lcd_cmd_args(uint8_t c, const uint8_t *args, uint32_t n);
void lcd_data(uint8_t d);
void lcd_data_buf(const uint8_t *p, uint32_t n);
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void lcd_fill(uint16_t color);

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#endif /* LCD_H */
