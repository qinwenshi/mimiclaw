#include "board_ui.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heartbeat/heartbeat.h"
#include "lcd_st7789.h"
#include "led_ws2812.h"
#include "wifi/wifi_manager.h"

static const char *TAG = "board_ui";

#define UI_FRAME_MS                 250
#define UI_BUTTON_STACK             4096
#define UI_RENDER_STACK             6144
#define UI_RENDER_PRIO              4
#define UI_BUTTON_PRIO              5
#define UI_DETAIL_LEN               96
#define UI_TITLE_LEN                24
#define UI_LINE_LEN                 80

typedef struct {
    board_ui_phase_t phase;
    char title[UI_TITLE_LEN];
    char detail[UI_DETAIL_LEN];
    char last_source[16];
    char last_rx[UI_LINE_LEN];
    char last_tx[UI_LINE_LEN];
    uint32_t inbound_count;
    uint32_t outbound_count;
    uint8_t page;
    uint8_t brightness_index;
    uint64_t last_activity_ms;
    uint64_t phase_since_ms;
} board_ui_state_t;

typedef struct {
    gpio_num_t pin;
    bool stable_pressed;
    bool last_sample_pressed;
    uint64_t last_change_ms;
    uint64_t press_start_ms;
    bool long_handled;
} button_state_t;

static portMUX_TYPE s_ui_lock = portMUX_INITIALIZER_UNLOCKED;
static board_ui_state_t s_ui = {
    .phase = BOARD_UI_PHASE_BOOT,
    .title = "BOOT",
    .detail = "BRINGING UP BOARD",
    .last_source = "SYSTEM",
    .last_rx = "WAITING",
    .last_tx = "WAITING",
    .brightness_index = 3,
};

static const uint8_t k_brightness_levels[] = {48, 112, 180, 255};
static uint16_t *s_framebuffer;
static bool s_ready;

static adc_oneshot_unit_handle_t s_adc1;
static adc_oneshot_unit_handle_t s_adc2;
static adc_cali_handle_t s_adc1_cali;
static adc_cali_handle_t s_adc2_cali;
static bool s_adc1_cali_enabled;
static bool s_adc2_cali_enabled;

static int s_battery_mv = 0;
static int s_battery_pct = -1;
static bool s_usb_present = false;
static uint64_t s_last_power_sample_ms;

static inline uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static inline uint16_t phase_bg(board_ui_phase_t phase)
{
    switch (phase) {
    case BOARD_UI_PHASE_BOOT:
        return rgb565(10, 18, 32);
    case BOARD_UI_PHASE_WIFI:
        return rgb565(38, 24, 8);
    case BOARD_UI_PHASE_ONBOARDING:
        return rgb565(36, 22, 12);
    case BOARD_UI_PHASE_READY:
        return rgb565(12, 24, 22);
    case BOARD_UI_PHASE_WORKING:
        return rgb565(8, 28, 34);
    case BOARD_UI_PHASE_REPLY:
        return rgb565(10, 34, 16);
    case BOARD_UI_PHASE_ERROR:
    default:
        return rgb565(42, 10, 10);
    }
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > BOARD_LCD_H_RES) {
        w = BOARD_LCD_H_RES - x;
    }
    if (y + h > BOARD_LCD_V_RES) {
        h = BOARD_LCD_V_RES - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int row = 0; row < h; row++) {
        uint16_t *dst = s_framebuffer + ((y + row) * BOARD_LCD_H_RES) + x;
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

static void fb_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

static void fb_draw_panel(int x, int y, int w, int h, uint16_t fill, uint16_t border)
{
    fb_fill_rect(x, y, w, h, fill);
    fb_draw_rect(x, y, w, h, border);
}

#define UI_FONT_W 5
#define UI_FONT_H 7
#define UI_FONT_ADV 6

typedef struct {
    char ch;
    uint8_t rows[UI_FONT_H];
} ui_glyph_t;

static const ui_glyph_t k_ui_font[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}},
    {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'J', {1, 1, 1, 1, 17, 17, 14}},
    {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'N', {17, 17, 25, 21, 19, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}},
    {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},
    {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 1, 14}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},
    {':', {0, 12, 12, 0, 12, 12, 0}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},
    {'/', {1, 2, 4, 8, 16, 0, 0}},
    {'_', {0, 0, 0, 0, 0, 0, 31}},
    {'?', {14, 17, 1, 2, 4, 0, 4}},
    {'!', {4, 4, 4, 4, 4, 0, 4}},
    {',', {0, 0, 0, 0, 12, 12, 8}},
    {'(', {2, 4, 8, 8, 8, 4, 2}},
    {')', {8, 4, 2, 2, 2, 4, 8}},
    {'[', {14, 8, 8, 8, 8, 8, 14}},
    {']', {14, 2, 2, 2, 2, 2, 14}},
    {'#', {10, 31, 10, 31, 10, 0, 0}},
    {'%', {17, 2, 4, 8, 17, 0, 0}},
    {'+', {0, 4, 4, 31, 4, 4, 0}},
    {'=', {0, 31, 0, 31, 0, 0, 0}},
    {' ', {0, 0, 0, 0, 0, 0, 0}},
};
static const uint8_t k_ui_glyph_fallback[UI_FONT_H] = {14, 17, 1, 2, 4, 0, 4};

static const uint8_t *ui_glyph_rows(char ch)
{
    for (size_t i = 0; i < sizeof(k_ui_font) / sizeof(k_ui_font[0]); i++) {
        if (k_ui_font[i].ch == ch) {
            return k_ui_font[i].rows;
        }
    }
    return k_ui_glyph_fallback;
}

static int fb_text_advance(int scale)
{
    return UI_FONT_ADV * scale;
}

static int fb_text_width_chars(size_t len, int scale)
{
    if (len == 0) {
        return 0;
    }
    return (int)(((len - 1) * (size_t)fb_text_advance(scale)) + (UI_FONT_W * scale));
}

static int fb_chars_for_width(int width, int scale)
{
    int min_width = UI_FONT_W * scale;
    int advance = fb_text_advance(scale);

    if (width <= min_width) {
        return 1;
    }
    return 1 + ((width - min_width) / advance);
}

static void fb_draw_char(int x, int y, char ch, int scale, uint16_t color)
{
    const uint8_t *rows = ui_glyph_rows(ch);

    for (int row = 0; row < UI_FONT_H; row++) {
        for (int col = 0; col < UI_FONT_W; col++) {
            if ((rows[row] >> (UI_FONT_W - 1 - col)) & 0x1) {
                fb_fill_rect(x + (col * scale), y + (row * scale), scale, scale, color);
            }
        }
    }
}

static void fb_draw_text_line(int x, int y, int scale, uint16_t color, uint16_t shadow_color,
                              const char *text)
{
    size_t len = strlen(text);
    int advance = fb_text_advance(scale);
    int shadow_offset = scale > 2 ? 2 : 1;

    for (size_t i = 0; i < len; i++) {
        int glyph_x = x + ((int)i * advance);
        if (shadow_color != color) {
            fb_draw_char(glyph_x + shadow_offset, y + shadow_offset, text[i], scale, shadow_color);
        }
        fb_draw_char(glyph_x, y, text[i], scale, color);
    }
}

static void ui_normalize_text(char *dst, size_t dst_len, const char *src)
{
    size_t j = 0;
    bool last_space = false;

    if (dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; src[i] != '\0' && j < dst_len - 1; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c < 0x20 || c > 0x7E) {
            c = ' ';
        }
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - 'a' + 'A');
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
        if (c == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }
        dst[j++] = (char)c;
    }

    while (j > 0 && dst[j - 1] == ' ') {
        j--;
    }
    dst[j] = '\0';
}

static size_t ui_copy_wrapped_line(char *dst, size_t dst_len, const char *src, size_t start,
                                   int max_chars, bool ellipsis)
{
    size_t len = strlen(src);
    size_t end;
    size_t last_space = SIZE_MAX;
    size_t out_len = 0;

    while (start < len && src[start] == ' ') {
        start++;
    }
    if (start >= len || max_chars <= 0 || dst_len == 0) {
        dst[0] = '\0';
        return len;
    }

    end = start;
    while (end < len && (int)(end - start) < max_chars) {
        if (src[end] == ' ') {
            last_space = end;
        }
        end++;
    }

    if (end < len && !ellipsis && last_space != SIZE_MAX && last_space > start) {
        end = last_space;
    }

    while (start < end && src[start] == ' ') {
        start++;
    }
    while (end > start && src[end - 1] == ' ') {
        end--;
    }

    out_len = end - start;
    if (out_len >= dst_len) {
        out_len = dst_len - 1;
    }
    memcpy(dst, src + start, out_len);
    dst[out_len] = '\0';

    if (ellipsis && end < len && max_chars >= 3) {
        size_t clip_len = strlen(dst);
        while (clip_len > 0 && dst[clip_len - 1] == ' ') {
            clip_len--;
        }
        if (clip_len > (size_t)(max_chars - 3)) {
            clip_len = (size_t)(max_chars - 3);
        }
        dst[clip_len] = '\0';
        strncat(dst, "...", dst_len - strlen(dst) - 1);
        return len;
    }

    if (last_space != SIZE_MAX && last_space >= end) {
        return last_space + 1;
    }
    return end;
}

static void fb_draw_text_clipped_ex(int x, int y, int scale, uint16_t color, uint16_t shadow_color,
                                    int max_chars, const char *text)
{
    char line[UI_LINE_LEN];
    size_t len;

    ui_normalize_text(line, sizeof(line), text);
    len = strlen(line);
    if (max_chars <= 0) {
        return;
    }
    if ((int)len > max_chars) {
        if (max_chars >= 3) {
            line[max_chars - 3] = '.';
            line[max_chars - 2] = '.';
            line[max_chars - 1] = '.';
            line[max_chars] = '\0';
            len = (size_t)max_chars;
        } else {
            line[max_chars] = '\0';
            len = (size_t)max_chars;
        }
    }

    fb_draw_text_line(x, y, scale, color, shadow_color, line);
}

static void fb_draw_text_centered_clipped_ex(int x, int y, int w, int scale, uint16_t color,
                                             uint16_t shadow_color, int max_chars, const char *text)
{
    char line[UI_LINE_LEN];
    size_t len;
    int text_x;

    ui_normalize_text(line, sizeof(line), text);
    len = strlen(line);
    if ((int)len > max_chars) {
        if (max_chars >= 3) {
            line[max_chars - 3] = '.';
            line[max_chars - 2] = '.';
            line[max_chars - 1] = '.';
        }
        line[max_chars] = '\0';
        len = (size_t)max_chars;
    }

    text_x = x + ((w - fb_text_width_chars(len, scale)) / 2);
    if (text_x < x) {
        text_x = x;
    }
    fb_draw_text_line(text_x, y, scale, color, shadow_color, line);
}

static void fb_draw_text_block_clipped_ex(int x, int y, int scale, uint16_t color,
                                          uint16_t shadow_color, int max_chars, int max_lines,
                                          const char *text)
{
    char normalized[UI_LINE_LEN];
    char line[UI_LINE_LEN];
    size_t pos = 0;

    if (max_chars <= 0 || max_lines <= 0) {
        return;
    }

    ui_normalize_text(normalized, sizeof(normalized), text);
    for (int line_idx = 0; line_idx < max_lines && normalized[pos] != '\0'; line_idx++) {
        bool ellipsis = line_idx == max_lines - 1;
        pos = ui_copy_wrapped_line(line, sizeof(line), normalized, pos, max_chars, ellipsis);
        fb_draw_text_line(x, y + (line_idx * (scale * 9)), scale, color, shadow_color, line);
        if (ellipsis) {
            break;
        }
    }
}

static int battery_percent_from_mv(int mv)
{
    static const struct {
        int mv;
        int pct;
    } table[] = {
        {1970, 0},
        {2062, 20},
        {2154, 40},
        {2246, 60},
        {2338, 80},
        {2430, 100},
    };

    if (mv <= table[0].mv) {
        return 0;
    }
    if (mv >= table[5].mv) {
        return 100;
    }

    for (size_t i = 1; i < sizeof(table) / sizeof(table[0]); i++) {
        if (mv <= table[i].mv) {
            int span_mv = table[i].mv - table[i - 1].mv;
            int span_pct = table[i].pct - table[i - 1].pct;
            int delta_mv = mv - table[i - 1].mv;
            return table[i - 1].pct + ((delta_mv * span_pct) / span_mv);
        }
    }

    return 100;
}

static bool adc_cali_init(adc_unit_t unit, adc_channel_t channel, adc_cali_handle_t *out_handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&curve_cfg, out_handle) == ESP_OK) {
        return true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_cfg = {
        .unit_id = unit,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&line_cfg, out_handle) == ESP_OK) {
        return true;
    }
#endif
    return false;
}

static esp_err_t power_sense_init(void)
{
    adc_oneshot_unit_init_cfg_t adc1_cfg = {
        .unit_id = BOARD_USB_SENSE_UNIT,
    };
    adc_oneshot_unit_init_cfg_t adc2_cfg = {
        .unit_id = BOARD_BATTERY_SENSE_UNIT,
    };
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc1_cfg, &s_adc1), TAG, "adc1 init failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc2_cfg, &s_adc2), TAG, "adc2 init failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc1, BOARD_USB_SENSE_CHANNEL, &chan_cfg),
                        TAG, "adc1 channel init failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc2, BOARD_BATTERY_SENSE_CHANNEL, &chan_cfg),
                        TAG, "adc2 channel init failed");

    s_adc1_cali_enabled = adc_cali_init(BOARD_USB_SENSE_UNIT, BOARD_USB_SENSE_CHANNEL, &s_adc1_cali);
    s_adc2_cali_enabled = adc_cali_init(BOARD_BATTERY_SENSE_UNIT, BOARD_BATTERY_SENSE_CHANNEL,
                                        &s_adc2_cali);
    return ESP_OK;
}

static int adc_to_mv(adc_oneshot_unit_handle_t unit, adc_channel_t channel,
                     adc_cali_handle_t cali, bool cali_enabled)
{
    int raw = 0;
    int mv = 0;

    if (adc_oneshot_read(unit, channel, &raw) != ESP_OK) {
        return 0;
    }
    if (cali_enabled && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
        return mv;
    }
    return (raw * 3300) / 4095;
}

static void power_sample_update(bool force)
{
    uint64_t now = now_ms();
    if (!force && now - s_last_power_sample_ms < 30000) {
        return;
    }

    int usb_mv = adc_to_mv(s_adc1, BOARD_USB_SENSE_CHANNEL, s_adc1_cali, s_adc1_cali_enabled);
    s_usb_present = usb_mv > 1000;

    gpio_set_direction(BOARD_LCD_RST_PIN, GPIO_MODE_DISABLE);
    esp_rom_delay_us(100);
    int batt_half_mv = adc_to_mv(s_adc2, BOARD_BATTERY_SENSE_CHANNEL, s_adc2_cali, s_adc2_cali_enabled);
    gpio_set_direction(BOARD_LCD_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LCD_RST_PIN, 1);

    s_battery_mv = batt_half_mv * 2;
    s_battery_pct = battery_percent_from_mv(batt_half_mv);
    s_last_power_sample_ms = now;
}

static void ui_touch_locked(uint64_t now)
{
    s_ui.last_activity_ms = now;
}

static void ui_touch(void)
{
    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_ui_lock);
    ui_touch_locked(now);
    portEXIT_CRITICAL(&s_ui_lock);
}

static uint8_t ui_current_backlight(void)
{
    uint64_t now = now_ms();
    uint64_t idle_ms;
    uint8_t base;

    portENTER_CRITICAL(&s_ui_lock);
    idle_ms = now - s_ui.last_activity_ms;
    base = k_brightness_levels[s_ui.brightness_index];
    portEXIT_CRITICAL(&s_ui_lock);

    if (idle_ms <= BOARD_BACKLIGHT_IDLE_TIMEOUT_MS) {
        return base;
    }
    idle_ms -= BOARD_BACKLIGHT_IDLE_TIMEOUT_MS;
    if (idle_ms >= BOARD_BACKLIGHT_FADE_MS) {
        return 0;
    }

    return (uint8_t)((uint32_t)base * (BOARD_BACKLIGHT_FADE_MS - idle_ms) / BOARD_BACKLIGHT_FADE_MS);
}

static void draw_led_for_phase(board_ui_phase_t phase, bool wifi_connected)
{
    uint64_t t = now_ms() / 125;
    uint8_t pulse = (uint8_t)(32 + ((t % 8) * 24));

    if (!wifi_connected && phase != BOARD_UI_PHASE_BOOT && phase != BOARD_UI_PHASE_ONBOARDING) {
        (void)led_set_color(pulse, pulse / 2, 0);
        return;
    }

    switch (phase) {
    case BOARD_UI_PHASE_BOOT:
        (void)led_set_color(0, 0, pulse);
        break;
    case BOARD_UI_PHASE_WIFI:
        (void)led_set_color(pulse, pulse / 3, 0);
        break;
    case BOARD_UI_PHASE_ONBOARDING:
        if (((t / 2) % 2) == 0) {
            (void)led_set_color(pulse, pulse / 4, 0);
        } else {
            (void)led_off();
        }
        break;
    case BOARD_UI_PHASE_READY:
        (void)led_set_color(0, 18, 4);
        break;
    case BOARD_UI_PHASE_WORKING:
        (void)led_set_color(0, pulse / 2, pulse);
        break;
    case BOARD_UI_PHASE_REPLY:
        (void)led_set_color(0, pulse, 0);
        break;
    case BOARD_UI_PHASE_ERROR:
    default:
        if ((t % 2) == 0) {
            (void)led_set_color(pulse, 0, 0);
        } else {
            (void)led_off();
        }
        break;
    }
}

static void render_metric_card(int x, int y, int w, int h, const char *label, const char *value,
                               const char *subline, uint16_t fill, uint16_t border,
                               uint16_t label_color, uint16_t value_color, uint16_t sub_color,
                               uint16_t shadow_color)
{
    fb_draw_panel(x, y, w, h, fill, border);
    fb_draw_text_clipped_ex(x + 10, y + 8, 1, label_color, shadow_color,
                            fb_chars_for_width(w - 20, 1), label);
    fb_draw_text_centered_clipped_ex(x + 8, y + 20, w - 16, 2, value_color, shadow_color,
                                     fb_chars_for_width(w - 16, 2), value);
    if (subline && subline[0]) {
        fb_draw_text_centered_clipped_ex(x + 8, y + h - 12, w - 16, 1, sub_color, shadow_color,
                                         fb_chars_for_width(w - 16, 1), subline);
    }
}

static void render_status_page(const board_ui_state_t *ui, bool wifi_connected, const char *ip)
{
    char battery_line[32];
    uint16_t bg = phase_bg(ui->phase);
    uint16_t header_bg = rgb565(16, 20, 28);
    uint16_t footer_bg = rgb565(14, 18, 26);
    uint16_t card = rgb565(18, 24, 34);
    uint16_t card_alt = rgb565(14, 19, 28);
    uint16_t white = rgb565(245, 248, 250);
    uint16_t sky = rgb565(160, 220, 255);
    uint16_t dim = rgb565(124, 142, 162);
    uint16_t line = rgb565(58, 72, 92);
    uint16_t shadow = rgb565(2, 6, 12);
    uint16_t accent = wifi_connected ? rgb565(70, 220, 120) : rgb565(235, 170, 20);
    const char *title = ui->title[0] ? ui->title : "READY";
    const char *detail = ui->detail[0] ? ui->detail : "AWAITING INPUT";
    const char *last_rx = ui->last_rx[0] ? ui->last_rx : "WAITING";
    const char *last_tx = ui->last_tx[0] ? ui->last_tx : "WAITING";

    fb_fill_rect(0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, bg);
    fb_fill_rect(0, 0, BOARD_LCD_H_RES, 28, header_bg);
    fb_fill_rect(0, BOARD_LCD_V_RES - 24, BOARD_LCD_H_RES, 24, footer_bg);
    fb_fill_rect(0, 28, BOARD_LCD_H_RES, 1, line);
    fb_fill_rect(0, BOARD_LCD_V_RES - 24, BOARD_LCD_H_RES, 1, line);

    fb_draw_text_clipped_ex(12, 8, 2, white, shadow, 14, "MIMICLAW");
    fb_draw_panel(154, 4, 74, 20,
                  wifi_connected ? rgb565(18, 52, 28) : rgb565(62, 42, 14), line);
    fb_draw_text_centered_clipped_ex(154, 10, 74, 1, accent, shadow, 10,
                                     wifi_connected ? "ONLINE" : "OFFLINE");

    fb_draw_panel(12, 38, 216, 72, card, line);
    fb_draw_text_clipped_ex(22, 48, 1, dim, shadow, 12, "CURRENT STATE");
    fb_draw_text_centered_clipped_ex(20, 62, 200, 3, white, shadow,
                                     fb_chars_for_width(200, 3), title);
    fb_draw_text_centered_clipped_ex(18, 86, 204, 2, sky, shadow,
                                     fb_chars_for_width(204, 2), detail);

    fb_draw_panel(12, 120, 216, 40, card_alt, line);
    fb_draw_text_clipped_ex(22, 128, 1, dim, shadow, 10, "LAST RX");
    fb_draw_text_block_clipped_ex(22, 140, 2, white, shadow, fb_chars_for_width(188, 2), 1, last_rx);

    fb_draw_panel(12, 168, 216, 40, card_alt, line);
    fb_draw_text_clipped_ex(22, 176, 1, dim, shadow, 10, "LAST TX");
    fb_draw_text_block_clipped_ex(22, 188, 2, white, shadow, fb_chars_for_width(188, 2), 1, last_tx);

    snprintf(battery_line, sizeof(battery_line), "BAT %d%%", s_battery_pct >= 0 ? s_battery_pct : 0);
    fb_draw_text_clipped_ex(10, 222, 1, sky, shadow, 10, battery_line);
    fb_draw_text_centered_clipped_ex(44, 222, 148, 1, white, shadow, 20, ip);
    fb_draw_text_clipped_ex(190, 222, 1, dim, shadow, 8, s_usb_present ? "USB P1" : "BAT P1");
}

static void render_stats_page(const board_ui_state_t *ui, bool wifi_connected, const char *ip)
{
    char source_value[32];
    char traffic_value[24];
    char traffic_sub[24];
    char memory_value[24];
    char memory_sub[24];
    char power_value[24];
    char power_sub[24];
    uint32_t heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint16_t bg = rgb565(10, 14, 24);
    uint16_t header_bg = rgb565(16, 20, 28);
    uint16_t footer_bg = rgb565(14, 18, 26);
    uint16_t card = rgb565(18, 24, 34);
    uint16_t white = rgb565(245, 248, 250);
    uint16_t sky = rgb565(160, 220, 255);
    uint16_t dim = rgb565(124, 142, 162);
    uint16_t line = rgb565(58, 72, 92);
    uint16_t shadow = rgb565(2, 6, 12);
    uint16_t accent = wifi_connected ? rgb565(70, 220, 120) : rgb565(235, 170, 20);

    fb_fill_rect(0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, bg);
    fb_fill_rect(0, 0, BOARD_LCD_H_RES, 28, header_bg);
    fb_fill_rect(0, BOARD_LCD_V_RES - 24, BOARD_LCD_H_RES, 24, footer_bg);
    fb_fill_rect(0, 28, BOARD_LCD_H_RES, 1, line);
    fb_fill_rect(0, BOARD_LCD_V_RES - 24, BOARD_LCD_H_RES, 1, line);

    fb_draw_text_clipped_ex(12, 8, 2, white, shadow, 14, "BOARD STATS");
    fb_draw_panel(154, 4, 74, 20,
                  wifi_connected ? rgb565(18, 52, 28) : rgb565(62, 42, 14), line);
    fb_draw_text_centered_clipped_ex(154, 10, 74, 1, accent, shadow, 10,
                                     wifi_connected ? "ONLINE" : "OFFLINE");

    fb_draw_panel(12, 38, 216, 46, card, line);
    fb_draw_text_clipped_ex(22, 48, 1, dim, shadow, 12, "STATUS");
    fb_draw_text_centered_clipped_ex(20, 60, 200, 2, white, shadow,
                                     fb_chars_for_width(200, 2), ui->title[0] ? ui->title : "READY");

    snprintf(source_value, sizeof(source_value), "%s", ui->last_source[0] ? ui->last_source : "NONE");
    snprintf(traffic_value, sizeof(traffic_value), "%lu/%lu",
             (unsigned long)ui->inbound_count, (unsigned long)ui->outbound_count);
    snprintf(traffic_sub, sizeof(traffic_sub), "IN / OUT");
    snprintf(memory_value, sizeof(memory_value), "%luK", (unsigned long)(heap_free / 1024));
    snprintf(memory_sub, sizeof(memory_sub), "PS %luK", (unsigned long)(psram_free / 1024));
    snprintf(power_value, sizeof(power_value), "%d%%", s_battery_pct >= 0 ? s_battery_pct : 0);
    snprintf(power_sub, sizeof(power_sub), "%d.%02dV %s",
             s_battery_mv / 1000, (s_battery_mv % 1000) / 10, s_usb_present ? "USB" : "BAT");

    render_metric_card(12, 96, 104, 52, "SOURCE", source_value, "", card, line, dim, white, sky, shadow);
    render_metric_card(124, 96, 104, 52, "TRAFFIC", traffic_value, traffic_sub, card, line,
                       dim, white, sky, shadow);
    render_metric_card(12, 156, 104, 52, "HEAP", memory_value, memory_sub, card, line,
                       dim, white, sky, shadow);
    render_metric_card(124, 156, 104, 52, "POWER", power_value, power_sub, card, line,
                       dim, white, sky, shadow);

    fb_draw_text_centered_clipped_ex(12, 222, 176, 1, sky, shadow, 24, ip);
    fb_draw_text_clipped_ex(194, 222, 1, dim, shadow, 6, "P2");
}

static void board_ui_render(void)
{
    board_ui_state_t ui_snapshot;
    bool wifi_connected;
    const char *ip;
    uint8_t backlight;

    power_sample_update(false);

    portENTER_CRITICAL(&s_ui_lock);
    ui_snapshot = s_ui;
    portEXIT_CRITICAL(&s_ui_lock);

    if (ui_snapshot.phase == BOARD_UI_PHASE_REPLY && now_ms() - ui_snapshot.phase_since_ms > 3000) {
        board_ui_set_phase(BOARD_UI_PHASE_READY, "READY", "AWAITING INPUT");
        portENTER_CRITICAL(&s_ui_lock);
        ui_snapshot = s_ui;
        portEXIT_CRITICAL(&s_ui_lock);
    } else if (ui_snapshot.phase == BOARD_UI_PHASE_ERROR &&
               now_ms() - ui_snapshot.phase_since_ms > 6000) {
        board_ui_set_phase(BOARD_UI_PHASE_READY, "READY", "RECOVERED");
        portENTER_CRITICAL(&s_ui_lock);
        ui_snapshot = s_ui;
        portEXIT_CRITICAL(&s_ui_lock);
    }

    wifi_connected = wifi_manager_is_connected();
    ip = wifi_manager_get_ip();
    backlight = ui_current_backlight();
    lcd_backlight_set(backlight);

    draw_led_for_phase(ui_snapshot.phase, wifi_connected);

    if (backlight == 0) {
        return;
    }

    if (ui_snapshot.page == 0) {
        render_status_page(&ui_snapshot, wifi_connected, ip);
    } else {
        render_stats_page(&ui_snapshot, wifi_connected, ip);
    }

    if (lcd_draw_bitmap(0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_framebuffer) != ESP_OK) {
        ESP_LOGW(TAG, "lcd update failed");
    }
}

static void board_ui_format_message(char *dst, size_t dst_len, const mimi_msg_t *msg)
{
    char text[UI_LINE_LEN];

    if (!msg) {
        dst[0] = '\0';
        return;
    }

    ui_normalize_text(text, sizeof(text), msg->content ? msg->content : "");
    if (text[0] == '\0') {
        snprintf(dst, dst_len, "%s", msg->channel);
        ui_normalize_text(dst, dst_len, dst);
        return;
    }

    snprintf(dst, dst_len, "%s %s", msg->channel, text);
    ui_normalize_text(dst, dst_len, dst);
}

static uint8_t board_ui_cycle_page(void)
{
    uint8_t page;
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.page ^= 1u;
    page = s_ui.page;
    ui_touch_locked(now_ms());
    portEXIT_CRITICAL(&s_ui_lock);
    return page;
}

static uint8_t board_ui_cycle_brightness(void)
{
    uint8_t level;
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.brightness_index = (uint8_t)((s_ui.brightness_index + 1u) %
                                      (sizeof(k_brightness_levels) / sizeof(k_brightness_levels[0])));
    level = s_ui.brightness_index;
    ui_touch_locked(now_ms());
    portEXIT_CRITICAL(&s_ui_lock);
    return level;
}

static void handle_short_press(gpio_num_t pin)
{
    if (pin == BOARD_BTN_BOOT_PIN) {
        uint8_t page = board_ui_cycle_page();
        board_ui_note_text("PAGE", page == 0 ? "STATUS" : "STATS");
    } else if (pin == BOARD_BTN_VOL_UP_PIN) {
        char detail[24];
        uint8_t level = board_ui_cycle_brightness();
        snprintf(detail, sizeof(detail), "LEVEL %u", (unsigned)(level + 1));
        board_ui_note_text("BRIGHT", detail);
    } else if (pin == BOARD_BTN_VOL_DOWN_PIN) {
        bool queued = heartbeat_trigger();
        if (queued) {
            board_ui_set_phase(BOARD_UI_PHASE_WORKING, "HEARTBEAT", "TASK CHECK QUEUED");
        } else {
            board_ui_note_text("HEARTBEAT", "NO ACTIONABLE TASKS");
        }
    } else if (pin == BOARD_BTN_POWER_PIN) {
        ui_touch();
        board_ui_note_text("WAKE", "DISPLAY RESTORED");
    }
}

static void handle_long_press(gpio_num_t pin)
{
    if (pin == BOARD_BTN_BOOT_PIN) {
        board_ui_set_phase(BOARD_UI_PHASE_BOOT, "RESTART", "BOOT BUTTON");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }
}

static void button_poll(button_state_t *btn)
{
    bool sample_pressed = gpio_get_level(btn->pin) == 0;
    uint64_t now = now_ms();

    if (sample_pressed != btn->last_sample_pressed) {
        btn->last_sample_pressed = sample_pressed;
        btn->last_change_ms = now;
    }

    if (now - btn->last_change_ms < BOARD_INPUT_DEBOUNCE_MS) {
        return;
    }

    if (sample_pressed == btn->stable_pressed) {
        if (btn->stable_pressed && btn->pin == BOARD_BTN_BOOT_PIN && !btn->long_handled &&
            now - btn->press_start_ms >= BOARD_BOOT_LONG_PRESS_MS) {
            btn->long_handled = true;
            handle_long_press(btn->pin);
        }
        return;
    }

    btn->stable_pressed = sample_pressed;
    ui_touch();

    if (sample_pressed) {
        btn->press_start_ms = now;
        btn->long_handled = false;
        return;
    }

    if (!btn->long_handled) {
        handle_short_press(btn->pin);
    }
}

static void board_buttons_task(void *arg)
{
    button_state_t buttons[] = {
        {.pin = BOARD_BTN_BOOT_PIN},
        {.pin = BOARD_BTN_VOL_UP_PIN},
        {.pin = BOARD_BTN_VOL_DOWN_PIN},
        {.pin = BOARD_BTN_POWER_PIN},
    };

    while (1) {
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            button_poll(&buttons[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(BOARD_INPUT_POLL_MS));
    }
}

static void board_render_task(void *arg)
{
    while (1) {
        board_ui_render();
        vTaskDelay(pdMS_TO_TICKS(UI_FRAME_MS));
    }
}

static esp_err_t buttons_init(void)
{
    gpio_config_t io_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = (1ULL << BOARD_BTN_BOOT_PIN) |
                        (1ULL << BOARD_BTN_VOL_UP_PIN) |
                        (1ULL << BOARD_BTN_VOL_DOWN_PIN) |
                        (1ULL << BOARD_BTN_POWER_PIN),
    };
    return gpio_config(&io_cfg);
}

esp_err_t board_ui_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "lcd init failed");
    ESP_RETURN_ON_ERROR(power_sense_init(), TAG, "power sensing init failed");
    ESP_RETURN_ON_ERROR(led_init(), TAG, "led init failed");
    ESP_RETURN_ON_ERROR(buttons_init(), TAG, "button init failed");

    s_framebuffer = heap_caps_malloc(BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_framebuffer) {
        s_framebuffer = heap_caps_malloc(BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_framebuffer != NULL, ESP_ERR_NO_MEM, TAG, "framebuffer alloc failed");

    power_sample_update(true);
    s_ui.last_activity_ms = now_ms();
    s_ui.phase_since_ms = s_ui.last_activity_ms;

    xTaskCreatePinnedToCore(board_render_task, "board_render", UI_RENDER_STACK, NULL,
                            UI_RENDER_PRIO, NULL, 1);
    xTaskCreatePinnedToCore(board_buttons_task, "board_buttons", UI_BUTTON_STACK, NULL,
                            UI_BUTTON_PRIO, NULL, 0);

    s_ready = true;
    ESP_LOGI(TAG, "Board UI ready for %s", BOARD_NAME);
    return ESP_OK;
}

void board_ui_set_phase(board_ui_phase_t phase, const char *title, const char *detail)
{
    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.phase = phase;
    s_ui.phase_since_ms = now;
    ui_touch_locked(now);
    ui_normalize_text(s_ui.title, sizeof(s_ui.title), title ? title : "");
    ui_normalize_text(s_ui.detail, sizeof(s_ui.detail), detail ? detail : "");
    portEXIT_CRITICAL(&s_ui_lock);
}

void board_ui_note_text(const char *title, const char *detail)
{
    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_ui_lock);
    ui_touch_locked(now);
    if (title && title[0]) {
        ui_normalize_text(s_ui.title, sizeof(s_ui.title), title);
    }
    if (detail) {
        ui_normalize_text(s_ui.detail, sizeof(s_ui.detail), detail);
    }
    portEXIT_CRITICAL(&s_ui_lock);
}

void board_ui_note_inbound(const mimi_msg_t *msg)
{
    char line[UI_LINE_LEN];
    if (!msg) {
        return;
    }

    board_ui_format_message(line, sizeof(line), msg);
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.inbound_count++;
    ui_touch_locked(now_ms());
    ui_normalize_text(s_ui.last_source, sizeof(s_ui.last_source), msg->channel);
    strncpy(s_ui.last_rx, line, sizeof(s_ui.last_rx) - 1);
    s_ui.last_rx[sizeof(s_ui.last_rx) - 1] = '\0';
    portEXIT_CRITICAL(&s_ui_lock);
}

void board_ui_note_outbound(const mimi_msg_t *msg)
{
    char line[UI_LINE_LEN];
    if (!msg) {
        return;
    }

    board_ui_format_message(line, sizeof(line), msg);
    portENTER_CRITICAL(&s_ui_lock);
    s_ui.outbound_count++;
    ui_touch_locked(now_ms());
    strncpy(s_ui.last_tx, line, sizeof(s_ui.last_tx) - 1);
    s_ui.last_tx[sizeof(s_ui.last_tx) - 1] = '\0';
    portEXIT_CRITICAL(&s_ui_lock);
}
