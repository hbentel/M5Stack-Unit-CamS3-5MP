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

/* Image settings applied at boot (PY260 driver levels, see config_mgr.c) */
typedef enum {
    CONFIG_IMG_BRIGHTNESS,   /* 0..8, 4 = neutral */
    CONFIG_IMG_CONTRAST,     /* 0..6, 3 = neutral */
    CONFIG_IMG_SATURATION,   /* 0..6, 3 = neutral */
    CONFIG_IMG_WB_MODE,      /* 0 Auto, 1 Sunny, 2 Office, 3 Cloudy, 4 Home */
    CONFIG_IMG_EFFECT,       /* 0 Normal ... 3 B&W ... 6 Greenish */
    CONFIG_IMG_COUNT
} config_img_t;

/* Getters */
const char *config_mgr_get_mqtt_url(void);
const char *config_mgr_get_mqtt_user(void);
const char *config_mgr_get_mqtt_pass(void);
const char *config_mgr_get_device_id(void);
bool        config_mgr_is_mqtt_enabled(void);
uint8_t     config_mgr_get_cam_resolution(void);   /* framesize_t value */
uint8_t     config_mgr_get_cam_quality(void);      /* 0 = High, 1 = Default, 2 = Low */
uint8_t     config_mgr_get_xclk_mhz(void);         /* camera XCLK: 10 (default), 16 or 20 */
uint8_t     config_mgr_get_image(config_img_t field);
uint8_t     config_mgr_image_default(config_img_t field);
uint8_t     config_mgr_image_max(config_img_t field);
const char *config_mgr_get_ota_token(void);
const char *config_mgr_get_coredump_token(void);

/* Setters (update in-memory state only — call config_mgr_stage_for_reboot() to persist) */
void config_mgr_set_mqtt_url(const char *v);
void config_mgr_set_mqtt_user(const char *v);
void config_mgr_set_mqtt_pass(const char *v);
void config_mgr_set_device_id(const char *v);
void config_mgr_set_mqtt_enabled(bool v);
void config_mgr_set_cam_resolution(uint8_t v);
void config_mgr_set_cam_quality(uint8_t v);
void config_mgr_set_xclk_mhz(uint8_t v);
void config_mgr_set_image(config_img_t field, uint8_t v);
void config_mgr_set_ota_token(const char *v);
void config_mgr_set_coredump_token(const char *v);

/**
 * @brief Stage the in-memory config in RTC RAM for saving on the next boot.
 *        Safe at any time (no flash write). The caller must then reboot
 *        (recovery_mgr_signal_planned_reboot() + esp_restart()).
 */
esp_err_t config_mgr_stage_for_reboot(void);

/**
 * @brief Stage a factory reset (erase all NVS, incl. Wi-Fi credentials) for the
 *        next boot. Safe at any time (no flash write). The caller must reboot.
 */
void config_mgr_stage_factory_reset(void);

/**
 * @brief Commit anything staged by the previous boot: save staged config to
 *        NVS, or erase NVS for a staged factory reset.
 *        MUST be called after nvs_flash_init() and BEFORE Wi-Fi starts — the
 *        only window where flash writes are known to be safe on this device.
 */
esp_err_t config_mgr_apply_staged(void);

#ifdef __cplusplus
}
#endif
