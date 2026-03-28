#include "led_ws2812.h"
#include "board.h"

#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "led_ws2812";

static led_strip_handle_t s_led;

esp_err_t led_init(void)
{
    if (s_led != NULL) {
        return ESP_OK;
    }

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_WS2812_PIN,
        .max_leds = BOARD_WS2812_MAX_LEDS,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led),
                        TAG, "led strip init failed");
    return led_off();
}

esp_err_t led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_RETURN_ON_FALSE(s_led != NULL, ESP_ERR_INVALID_STATE, TAG, "led not initialized");
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led, 0, r, g, b), TAG, "set pixel failed");
    ESP_RETURN_ON_ERROR(led_strip_refresh(s_led), TAG, "refresh failed");
    return ESP_OK;
}

esp_err_t led_off(void)
{
    ESP_RETURN_ON_FALSE(s_led != NULL, ESP_ERR_INVALID_STATE, TAG, "led not initialized");
    return led_strip_clear(s_led);
}
