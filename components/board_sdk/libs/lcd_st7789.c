#include "lcd_st7789.h"
#include "board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"

static const char *TAG = "lcd_st7789";

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_dma_buf[2];
static size_t s_dma_buf_pixels;
static uint8_t s_backlight = 255;
static size_t s_next_dma_index;
static bool s_ready;

static esp_err_t lcd_reset_panel(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOARD_LCD_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "reset gpio init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_LCD_RST_PIN, 0), TAG, "panel reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_LCD_RST_PIN, 1), TAG, "panel reset release failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

static esp_err_t lcd_init_backlight(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOARD_LCD_BL_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "backlight gpio init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_LCD_BL_PIN, 1), TAG, "backlight precharge failed");

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "backlight timer init failed");

    ledc_channel_config_t channel_cfg = {
        .gpio_num = BOARD_LCD_BL_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "backlight channel init failed");
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    return ESP_OK;
}

esp_err_t lcd_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOARD_LCD_SCLK_PIN,
        .mosi_io_num = BOARD_LCD_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = BOARD_LCD_H_RES * BOARD_LCD_BAND_ROWS * sizeof(uint16_t) + 16,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS_PIN,
        .dc_gpio_num = BOARD_LCD_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = BOARD_LCD_CMD_BITS,
        .lcd_param_bits = BOARD_LCD_PARAM_BITS,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_cfg, &s_panel_io),
        TAG, "panel io init failed");

    ESP_RETURN_ON_ERROR(lcd_reset_panel(), TAG, "panel hard reset failed");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel),
                        TAG, "panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_panel_io, LCD_CMD_SWRESET, NULL, 0), TAG,
                        "panel sw reset failed");
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel hw init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "panel invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_panel_io, LCD_CMD_NORON, NULL, 0), TAG,
                        "panel normal mode failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "panel gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel enable failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(lcd_init_backlight(), TAG, "backlight init failed");

    s_dma_buf_pixels = BOARD_LCD_H_RES * BOARD_LCD_BAND_ROWS;
    for (size_t i = 0; i < 2; i++) {
        s_dma_buf[i] = heap_caps_malloc(
            s_dma_buf_pixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(s_dma_buf[i] != NULL, ESP_ERR_NO_MEM, TAG,
                            "failed to alloc dma buffer %u", (unsigned)i);
    }

    s_ready = true;
    lcd_backlight_set(255);
    ESP_LOGI(TAG, "LCD ready: %dx%d", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

static inline bool lcd_clip_rect(int *x, int *y, int *w, int *h)
{
    if (*w <= 0 || *h <= 0) {
        return false;
    }
    if (*x >= BOARD_LCD_H_RES || *y >= BOARD_LCD_V_RES) {
        return false;
    }
    if (*x + *w <= 0 || *y + *h <= 0) {
        return false;
    }

    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > BOARD_LCD_H_RES) {
        *w = BOARD_LCD_H_RES - *x;
    }
    if (*y + *h > BOARD_LCD_V_RES) {
        *h = BOARD_LCD_V_RES - *y;
    }
    return *w > 0 && *h > 0;
}

static uint16_t *lcd_acquire_dma_buf(void)
{
    uint16_t *buf = s_dma_buf[s_next_dma_index];
    s_next_dma_index = (s_next_dma_index + 1) % 2;
    return buf;
}

esp_err_t lcd_draw_bitmap(int x, int y, int w, int h, const uint16_t *data)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "lcd not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "bitmap is null");

    if (!lcd_clip_rect(&x, &y, &w, &h)) {
        return ESP_OK;
    }

    size_t max_rows = s_dma_buf_pixels / (size_t)w;
    ESP_RETURN_ON_FALSE(max_rows > 0, ESP_ERR_INVALID_ARG, TAG, "bitmap width too large");

    int rows_sent = 0;
    while (rows_sent < h) {
        int chunk_rows = h - rows_sent;
        if ((size_t)chunk_rows > max_rows) {
            chunk_rows = (int)max_rows;
        }

        uint16_t *dma_buf = lcd_acquire_dma_buf();
        for (int row = 0; row < chunk_rows; row++) {
            memcpy(dma_buf + (row * w),
                   data + ((rows_sent + row) * w),
                   (size_t)w * sizeof(uint16_t));
        }

        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(s_panel, x, y + rows_sent, x + w, y + rows_sent + chunk_rows,
                                      dma_buf),
            TAG, "bitmap push failed");
        rows_sent += chunk_rows;
    }

    return ESP_OK;
}

esp_err_t lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "lcd not initialized");

    if (!lcd_clip_rect(&x, &y, &w, &h)) {
        return ESP_OK;
    }

    size_t max_rows = s_dma_buf_pixels / (size_t)w;
    ESP_RETURN_ON_FALSE(max_rows > 0, ESP_ERR_INVALID_ARG, TAG, "fill width too large");

    int rows_sent = 0;
    while (rows_sent < h) {
        int chunk_rows = h - rows_sent;
        if ((size_t)chunk_rows > max_rows) {
            chunk_rows = (int)max_rows;
        }

        uint16_t *dma_buf = lcd_acquire_dma_buf();
        size_t pixels = (size_t)w * (size_t)chunk_rows;
        for (size_t i = 0; i < pixels; i++) {
            dma_buf[i] = color;
        }

        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(s_panel, x, y + rows_sent, x + w, y + rows_sent + chunk_rows,
                                      dma_buf),
            TAG, "fill push failed");
        rows_sent += chunk_rows;
    }

    return ESP_OK;
}

esp_err_t lcd_fill_screen(uint16_t color)
{
    return lcd_fill_rect(0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, color);
}

void lcd_backlight_set(uint8_t brightness)
{
    if (!s_ready) {
        return;
    }
    s_backlight = brightness;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint8_t lcd_backlight_get(void)
{
    return s_backlight;
}
