#include "recovery_mgr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"

static const char *TAG = "recovery";

#define PLANNED_REBOOT_MAGIC 0xC0DEABC1UL
static RTC_NOINIT_ATTR uint32_t s_planned_reboot_magic;

void recovery_mgr_signal_planned_reboot(void)
{
    s_planned_reboot_magic = PLANNED_REBOOT_MAGIC;
}

static recovery_config_t s_config = {
    .enable_reboot = true,
    .enable_safe_mode = true,
    .max_reinit_attempts = 3,
    .bootloop_threshold = 3
};

static bool s_safe_mode = false;
static int s_reinit_count = 0;
static int s_total_errors = 0;
static int64_t s_last_error_time = 0;
static esp_timer_handle_t s_health_timer = NULL;

#define ERROR_WINDOW_US   60000000   // 1 minute window for error counting
#define HEALTHY_DELAY_US  120000000  // 2 minutes before marking healthy

// Timer callback — fires once after 2 minutes of stable uptime
static void health_timer_cb(void *arg)
{
    recovery_mgr_mark_healthy();
}

esp_err_t recovery_mgr_init(const recovery_config_t *config)
{
    if (config) {
        s_config = *config;
    }

    // Open NVS
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open("recovery", NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS");
        return err;
    }

    // Check Boot Loop
    int32_t boot_count = 0;
    nvs_get_i32(nvs_h, "boot_count", &boot_count);

    if (s_planned_reboot_magic == PLANNED_REBOOT_MAGIC) {
        s_planned_reboot_magic = 0;
        boot_count = 0;
        ESP_LOGI(TAG, "Planned reboot — boot counter reset");
    }

    boot_count++;
    nvs_set_i32(nvs_h, "boot_count", boot_count);
    nvs_commit(nvs_h);

    ESP_LOGI(TAG, "Boot count: %ld", (long)boot_count);

    if (s_config.enable_safe_mode && boot_count >= s_config.bootloop_threshold) {
        ESP_LOGE(TAG, "BOOT LOOP DETECTED (%ld boots)! Entering Safe Mode.", (long)boot_count);
        s_safe_mode = true;
    }

    nvs_close(nvs_h);

    // Start one-shot timer to mark healthy after 2 minutes
    const esp_timer_create_args_t timer_args = {
        .callback = health_timer_cb,
        .name = "recovery_health"
    };
    err = esp_timer_create(&timer_args, &s_health_timer);
    if (err == ESP_OK) {
        esp_timer_start_once(s_health_timer, HEALTHY_DELAY_US);
        ESP_LOGI(TAG, "Health timer started (2 min)");
    }

    return ESP_OK;
}

void recovery_mgr_mark_healthy(void)
{
    // Confirm OTA image is valid — prevents auto-rollback on next reboot
    esp_ota_mark_app_valid_cancel_rollback();

    nvs_handle_t nvs_h;
    if (nvs_open("recovery", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_i32(nvs_h, "boot_count", 0);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
        ESP_LOGI(TAG, "System marked healthy. Boot counter reset. OTA rollback cancelled.");
    }

    // Also clear safe mode if we've been stable
    if (s_safe_mode) {
        ESP_LOGW(TAG, "Clearing safe mode — system is stable.");
        s_safe_mode = false;
    }
}

void recovery_mgr_report_error(recovery_error_t error)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_error_time > ERROR_WINDOW_US) {
        s_reinit_count = 0;
    }
    s_last_error_time = now;

    ESP_LOGW(TAG, "Error reported: %d (count: %d/%d)",
             error, s_reinit_count + 1, s_config.max_reinit_attempts);

    switch (error) {
    case RECOVERY_ERR_FRAME_TIMEOUT:
    case RECOVERY_ERR_STREAM_SEND:
        s_reinit_count++;
        s_total_errors++;
        if (s_reinit_count > s_config.max_reinit_attempts) {
            ESP_LOGE(TAG, "Too many failures (%d). Escalating...", s_reinit_count);

            if (s_config.enable_reboot) {
                ESP_LOGE(TAG, "Triggering System Restart.");
                esp_restart();
            }
        }
        break;

    default:
        break;
    }
}

bool recovery_mgr_is_safe_mode(void)
{
    return s_safe_mode;
}

int recovery_mgr_get_error_count(void)
{
    return s_total_errors;
}
