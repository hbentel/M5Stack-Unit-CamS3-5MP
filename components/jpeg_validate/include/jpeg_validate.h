#pragma once

#include <stdbool.h>
#include "esp_camera.h"

/**
 * @brief Validate JPEG frame has proper SOI/EOI markers and sane length.
 * Increments internal drop counter on failure.
 *
 * @param fb Frame buffer from esp_camera_fb_get()
 * @return true if frame is valid JPEG, false otherwise
 */
bool jpeg_validate_frame(const camera_fb_t *fb);

/**
 * @brief Get the number of frames dropped due to validation failure since boot.
 */
uint32_t jpeg_validate_get_drop_count(void);
