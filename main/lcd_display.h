#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t lcd_display_init(void);
esp_err_t lcd_display_draw_bitmap(int x_start, int y_start, int x_end, int y_end, const void *color_data);
esp_err_t lcd_display_set_brightness(uint8_t brightness);
uint8_t lcd_display_get_brightness(void);
int lcd_display_width(void);
int lcd_display_height(void);

#endif /* LCD_DISPLAY_H */
