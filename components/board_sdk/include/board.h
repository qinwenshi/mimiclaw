#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"

#define BOARD_NAME                    "esp32-1.54"

#define BOARD_LCD_HOST                SPI2_HOST
#define BOARD_LCD_H_RES               240
#define BOARD_LCD_V_RES               240
#define BOARD_LCD_PIXEL_CLOCK_HZ      (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS            8
#define BOARD_LCD_PARAM_BITS          8
#define BOARD_LCD_BAND_ROWS           20

#define BOARD_LCD_MOSI_PIN            GPIO_NUM_10
#define BOARD_LCD_SCLK_PIN            GPIO_NUM_9
#define BOARD_LCD_CS_PIN              GPIO_NUM_14
#define BOARD_LCD_DC_PIN              GPIO_NUM_8
#define BOARD_LCD_RST_PIN             GPIO_NUM_18
#define BOARD_LCD_BL_PIN              GPIO_NUM_13

#define BOARD_WS2812_PIN              GPIO_NUM_48
#define BOARD_WS2812_MAX_LEDS         1

#define BOARD_BTN_BOOT_PIN            GPIO_NUM_0
#define BOARD_BTN_VOL_UP_PIN          GPIO_NUM_40
#define BOARD_BTN_VOL_DOWN_PIN        GPIO_NUM_39
#define BOARD_BTN_POWER_PIN           GPIO_NUM_47

#define BOARD_USB_SENSE_PIN           GPIO_NUM_1
#define BOARD_USB_SENSE_UNIT          ADC_UNIT_1
#define BOARD_USB_SENSE_CHANNEL       ADC_CHANNEL_0

#define BOARD_BATTERY_SENSE_PIN       GPIO_NUM_18
#define BOARD_BATTERY_SENSE_UNIT      ADC_UNIT_2
#define BOARD_BATTERY_SENSE_CHANNEL   ADC_CHANNEL_7

#define BOARD_INPUT_POLL_MS           10
#define BOARD_INPUT_DEBOUNCE_MS       300
#define BOARD_BOOT_LONG_PRESS_MS      3000

#define BOARD_BACKLIGHT_IDLE_TIMEOUT_MS   (5 * 60 * 1000)
#define BOARD_BACKLIGHT_FADE_MS           (30 * 1000)
