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
 */
void mqtt_mgr_register_ota_callback(esp_err_t (*cb)(const char *url));

#ifdef __cplusplus
}
#endif
