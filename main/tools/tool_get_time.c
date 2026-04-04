#include "tools/tool_get_time.h"
#include "mimi_config.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "tool_get_time";

static esp_err_t sync_time_via_sntp(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(MIMI_NTP_SERVER);

    esp_netif_sntp_deinit();

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SNTP with %s: %s", MIMI_NTP_SERVER, esp_err_to_name(err));
        return err;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(MIMI_NTP_SYNC_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Timed out syncing time via %s: %s", MIMI_NTP_SERVER, esp_err_to_name(err));
        esp_netif_sntp_deinit();
        return err;
    }

    esp_netif_sntp_deinit();
    ESP_LOGI(TAG, "Time synced via %s", MIMI_NTP_SERVER);
    return ESP_OK;
}

esp_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = sync_time_via_sntp();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to sync time via %s (%s)",
                 MIMI_NTP_SERVER, esp_err_to_name(err));
        return err;
    }

    setenv("TZ", MIMI_TIMEZONE, 1);
    tzset();

    time_t now = time(NULL);
    struct tm local_time = {0};
    if (localtime_r(&now, &local_time) == NULL) {
        snprintf(output, output_size, "Error: failed to read local time");
        return ESP_FAIL;
    }

    if (strftime(output, output_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &local_time) == 0) {
        snprintf(output, output_size, "Error: output buffer too small");
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}
