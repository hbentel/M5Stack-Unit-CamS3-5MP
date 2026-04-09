/* wifi_v6.c — Wi-Fi init + BLE provisioning for ESP-IDF 6.0+
 *
 * Drop-in replacement for wifi.c. Selected at compile time by CMakeLists.txt
 * when IDF_VERSION_MAJOR >= 6. Uses network_provisioning instead of the
 * removed wifi_provisioning component.
 *
 * Public API is identical to wifi.c (wifi.h is shared).
 */
#include "wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <network_provisioning/manager.h>
#include <network_provisioning/scheme_ble.h>
#include "esp_attr.h"
#include "recovery_mgr.h"

static const char *TAG = "wifi";
static uint32_t s_disconnect_count = 0;
static char s_ip_addr[16] = {0};
static volatile bool s_provisioning_active = false;

// ========================================
// Re-provisioning via RTC_NOINIT (same pattern as OTA URL)
// ========================================
RTC_NOINIT_ATTR static uint32_t s_reprovision_magic;
#define REPROVISION_MAGIC 0xDEADB0B0U

void wifi_start_reprovision(void)
{
    s_reprovision_magic = REPROVISION_MAGIC;
    recovery_mgr_signal_planned_reboot(); // Prevents boot-loop counter from tripping
    ESP_LOGW(TAG, "Reprovision requested — rebooting into BLE provisioning...");
    esp_restart();
}

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_START:
                ESP_LOGI(TAG, "Provisioning started — open Espressif Provisioning app");
                ESP_LOGI(TAG, "  Device: PROV_%s", CONFIG_UNITCAMS3_DEVICE_ID);
                break;
            case NETWORK_PROV_WIFI_CRED_RECV: {
                wifi_sta_config_t *cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "Received credentials for SSID: %s", (char *)cfg->ssid);
                break;
            }
            case NETWORK_PROV_WIFI_CRED_FAIL: {
                network_prov_wifi_sta_fail_reason_t *reason =
                    (network_prov_wifi_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed: %s — try again in the app",
                    (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ? "auth error" : "AP not found");
                break;
            }
            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning credentials verified — connected");
                break;
            case NETWORK_PROV_END:
                s_provisioning_active = false;
                network_prov_mgr_deinit();
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_provisioning_active) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_provisioning_active) {
            wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
            s_disconnect_count++;
            ESP_LOGW(TAG, "Disconnected (reason: %d). Retrying in 1s...", evt->reason);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_addr);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP address — triggering reconnect");
        memset(s_ip_addr, 0, sizeof(s_ip_addr));
        esp_wifi_disconnect();
        esp_wifi_connect();
    }
}

void wifi_get_stats(wifi_stats_t *stats)
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        stats->rssi = info.rssi;
    } else {
        stats->rssi = 0;
    }
    stats->disconnect_count = s_disconnect_count;
    strncpy(stats->ip, s_ip_addr, sizeof(stats->ip));
}

esp_err_t wifi_init_sta(void)
{
    // Check for reprovision request written by wifi_start_reprovision() on the previous boot.
    // The magic survives esp_restart() via RTC_NOINIT_ATTR (no-load section, bootloader skips it).
    bool reprovision = (s_reprovision_magic == REPROVISION_MAGIC);
    if (reprovision) {
        s_reprovision_magic = 0; // Clear before any potential crash
        ESP_LOGW(TAG, "Reprovision magic detected — will clear Wi-Fi credentials");
    }

    // NVS is required by Wi-Fi driver and provisioning manager
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                        &event_handler, NULL, NULL));

    // Init provisioning manager with NimBLE scheme.
    // NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE releases BLE memory after provisioning,
    // recovering ~50KB of heap for the camera/HTTP stack.
    network_prov_mgr_config_t prov_config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_config));

    if (reprovision) {
        // IDF v6 equivalent of wifi_prov_mgr_reset_provisioning() — clears stored SSID/password
        // via esp_wifi_restore() internally. NVS write is safe here: camera DMA not yet started.
        network_prov_mgr_reset_wifi_provisioning();
        ESP_LOGW(TAG, "Wi-Fi credentials cleared — entering BLE provisioning");
    }

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        ESP_LOGI(TAG, "No Wi-Fi credentials found — starting BLE provisioning");
        s_provisioning_active = true;
        // Manager starts Wi-Fi internally and handles the STA connect cycle
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1, NULL, "PROV_" CONFIG_UNITCAMS3_DEVICE_ID, NULL));
        network_prov_mgr_wait(); // blocks until NETWORK_PROV_END (credentials verified)
        ESP_LOGI(TAG, "Provisioning complete");
    } else {
        ESP_LOGI(TAG, "Found Wi-Fi credentials — connecting directly");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        // STA_START event fires → event_handler calls esp_wifi_connect()
    }

    // Disable power save for stable XCLK timing; full TX power for reliable uplink.
    // Do NOT cap TX power with esp_wifi_set_max_tx_power() — 8 dBm cap was the
    // root cause of 105 Wi-Fi disconnects (16× reduction in uplink power).
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Waiting for IP address...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    return ESP_OK;
}
