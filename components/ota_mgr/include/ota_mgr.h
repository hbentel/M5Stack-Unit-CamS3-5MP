#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize OTA manager.
 *
 * Registers the URL-based OTA callback with mqtt_mgr.
 * Trigger OTA by publishing a firmware URL to unitcams3/ota/set.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t ota_mgr_init(void);

/**
 * @brief Save OTA URL to NVS and reboot immediately.
 *
 * Called from MQTT on receipt of unitcams3/ota/set.
 * Saves the URL to NVS, then calls esp_restart().
 * On the next boot, ota_mgr_run_pending() picks up the URL and
 * runs the OTA before the camera is ever initialized.
 *
 * @param url HTTP URL pointing to the firmware binary
 * @return Does not return on success (reboots). Returns error code on failure.
 */
esp_err_t ota_mgr_start_url(const char *url);

/**
 * @brief Check NVS for a pending OTA URL and run it if found.
 *
 * Must be called from main() AFTER Wi-Fi connects but BEFORE
 * esp_camera_init(). If a pending URL is found, downloads and
 * flashes the firmware, then reboots into the new image.
 * Does not return if OTA is pending (reboots on success or failure).
 * Returns ESP_OK immediately if no pending OTA is found.
 *
 * @return ESP_OK if no pending OTA (normal boot continues)
 */
esp_err_t ota_mgr_run_pending(void);

#ifdef __cplusplus
}
#endif
