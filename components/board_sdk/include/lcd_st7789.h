#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t lcd_init(void);
esp_err_t lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
esp_err_t lcd_fill_screen(uint16_t color);
esp_err_t lcd_draw_bitmap(int x, int y, int w, int h, const uint16_t *data);
void lcd_backlight_set(uint8_t brightness);
uint8_t lcd_backlight_get(void);
