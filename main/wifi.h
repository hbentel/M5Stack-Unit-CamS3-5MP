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

/**
 * @brief Erase Wi-Fi credentials and reboot into BLE provisioning.
 *
 * Stores a magic value in RTC_NOINIT memory (survives esp_restart()),
 * signals a planned reboot (prevents boot-loop counter from tripping),
 * then calls esp_restart(). On the next boot, wifi_init_sta() detects
 * the magic and calls wifi_prov_mgr_reset_provisioning() before the
 * provisioning check, causing the device to enter BLE provisioning mode.
 *
 * Safe to call while camera DMA is running: uses RTC SRAM only, no flash write.
 * The NVS erase happens on the next boot, before esp_camera_init().
 */
void wifi_start_reprovision(void);
