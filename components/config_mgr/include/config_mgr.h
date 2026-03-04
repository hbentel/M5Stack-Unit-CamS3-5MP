#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load config from NVS, falling back to Kconfig defaults.
 *        Read-only NVS access — safe to call at any boot stage.
 */
esp_err_t config_mgr_init(void);

/* Getters */
const char *config_mgr_get_mqtt_url(void);
const char *config_mgr_get_mqtt_user(void);
const char *config_mgr_get_mqtt_pass(void);
const char *config_mgr_get_device_id(void);
bool        config_mgr_is_mqtt_enabled(void);
uint8_t     config_mgr_get_cam_resolution(void);   /* framesize_t value */
uint8_t     config_mgr_get_jpeg_quality(void);

/* Setters (update in-memory state only — call config_mgr_save() to persist) */
void config_mgr_set_mqtt_url(const char *v);
void config_mgr_set_mqtt_user(const char *v);
void config_mgr_set_mqtt_pass(const char *v);
void config_mgr_set_device_id(const char *v);
void config_mgr_set_mqtt_enabled(bool v);
void config_mgr_set_cam_resolution(uint8_t v);
void config_mgr_set_jpeg_quality(uint8_t v);

/**
 * @brief Write all fields to NVS.
 *        MUST only be called after esp_camera_deinit() — any flash write
 *        disables the OPI PSRAM cache and will fault if camera DMA is running.
 */
esp_err_t config_mgr_save(void);

#ifdef __cplusplus
}
#endif
