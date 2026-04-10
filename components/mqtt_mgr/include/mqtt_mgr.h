#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the MQTT Manager.
 * 
 * @param broker_url MQTT Broker URL (e.g., "mqtt://192.168.1.100")
 * @param username MQTT Username
 * @param password MQTT Password
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mqtt_mgr_start(const char *broker_url, const char *username, const char *password, const char *device_id);

/**
 * @brief Publish a metric value to a subtopic.
 * 
 * @param topic_suffix Suffix for the topic (e.g., "rssi")
 * @param value String value to publish
 * @return esp_err_t 
 */
esp_err_t mqtt_mgr_publish(const char *topic_suffix, const char *value);

/**
 * @brief Stop the MQTT Manager (disconnects and stops tasks).
 */
void mqtt_mgr_stop(void);

/**
 * @brief Register a callback for OTA URL commands received via MQTT.
 *        Called by ota_mgr_init() to avoid a circular link dependency.
 *        sha256_hex may be NULL if no hash was provided in the payload.
 */
void mqtt_mgr_register_ota_callback(esp_err_t (*cb)(const char *url, const char *sha256_hex));

/**
 * @brief Register a callback for Wi-Fi re-provisioning commands received via MQTT.
 *        Called by main.c after mqtt_mgr_start() to avoid a circular link dependency.
 *        The callback should call wifi_start_reprovision() which writes RTC_NOINIT magic
 *        and calls esp_restart() — no return expected.
 */
void mqtt_mgr_register_reprovision_callback(void (*cb)(void));

/**
 * @brief Register a callback for LED on/off commands received via MQTT.
 *        Called by main.c after mqtt_mgr_start(). The callback receives 0 (off) or 1 (on).
 *        Keeps GPIO access in main.c, avoiding esp_driver_gpio dependency in mqtt_mgr.
 */
void mqtt_mgr_register_led_callback(void (*cb)(int level));

#ifdef __cplusplus
}
#endif
