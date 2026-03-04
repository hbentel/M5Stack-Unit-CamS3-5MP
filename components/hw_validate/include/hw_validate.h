#pragma once

#include "esp_err.h"

/**
 * Validate OPI PSRAM is detected and usable.
 * Returns ESP_OK if PSRAM is initialized and reports expected size.
 */
esp_err_t hw_validate_psram(void);

