#include "hw_validate.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

static const char *TAG = "hw_validate";

/* --------------------------------------------------------------------------
 * UnitCamS3-5MP I2C pin definitions (from M5Stack schematic)
 * -------------------------------------------------------------------------- */
#define CAM_I2C_SDA_PIN     17
#define CAM_I2C_SCL_PIN     41
#define CAM_I2C_FREQ_HZ     400000

/* PY260 default I2C slave address */
#define PY260_I2C_ADDR       0x1F

/* -------------------------------------------------------------------------- */

esp_err_t hw_validate_psram(void)
{
    size_t psram_size = esp_psram_get_size();
    if (psram_size == 0) {
        ESP_LOGE(TAG, "PSRAM not detected! Check OPI PSRAM config.");
        return ESP_FAIL;
    }

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    ESP_LOGI(TAG, "PSRAM detected: %zu bytes total, %zu bytes free", psram_size, free_psram);
    ESP_LOGI(TAG, "Internal SRAM free: %zu bytes", free_internal);

    return ESP_OK;
}

