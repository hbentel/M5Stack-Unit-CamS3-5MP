#pragma once

#include "esp_err.h"

/**
 * @brief Initializes NVS, Wi-Fi, and blocks until an IP is acquired.
 *
 * On first boot (no credentials in NVS): starts BLE provisioning.
 * The device advertises as "PROV_unitcams3" — open the Espressif
 * Provisioning app, select the device, and enter your SSID/password.
 * Credentials are stored in NVS and BLE is released after provisioning.
 *
 * On subsequent boots: connects directly using stored credentials.
 */
esp_err_t wifi_init_sta(void);

typedef struct {
    int rssi;
    uint32_t disconnect_count;
    char ip[16];
} wifi_stats_t;

void wifi_get_stats(wifi_stats_t *stats);
