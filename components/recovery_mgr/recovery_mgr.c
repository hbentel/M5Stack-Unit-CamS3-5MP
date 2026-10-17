#include "recovery_mgr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"

static const char *TAG = "recovery";

/*
 * Crash-loop detection without flash writes.
 *
 * Every flash write disables the OPI PSRAM cache, and doing one after Wi-Fi starts
 * has crashed this device, so the boot counter lives in RTC_NOINIT memory instead
 * of NVS. RTC_NOINIT survives panics, watchdog resets and esp_restart() (but not
 * power-on), which is exactly the set of resets a crash loop produces.
 *
 * A boot counts as a crash if the reset reason is a panic/watchdog, or if the
 * previous boot restarted itself via recovery_mgr_report_error() escalation.
 * recovery_mgr_mark_stable() (called after a stable period) clears the count.
 */
#define RTC_MAGIC                 0x5AFE600DUL
#define PLANNED_REBOOT_MAGIC      0xC0DEABC1UL
#define ERROR_RESTART_MAGIC       0xE5505A1EUL

static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR uint32_t s_crash_count;
static RTC_NOINIT_ATTR uint32_t s_planned_reboot_magic;
static RTC_NOINIT_ATTR uint32_t s_error_restart_magic;

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

#define ERROR_WINDOW_US   60000000   // 1 minute window for error counting

esp_err_t recovery_mgr_init(const recovery_config_t *config)
{
    if (config) {
        s_config = *config;
    }

    if (s_rtc_magic != RTC_MAGIC) {      // power-on: RTC_NOINIT holds garbage
        s_rtc_magic = RTC_MAGIC;
        s_crash_count = 0;
        s_planned_reboot_magic = 0;
        s_error_restart_magic = 0;
    }

    esp_reset_reason_t rr = esp_reset_reason();
    bool planned = (s_planned_reboot_magic == PLANNED_REBOOT_MAGIC);
    bool crashed = !planned && (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                                rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                                s_error_restart_magic == ERROR_RESTART_MAGIC);
    s_planned_reboot_magic = 0;
    s_error_restart_magic = 0;

    if (crashed) {
        s_crash_count++;
    } else if (rr == ESP_RST_POWERON) {
        s_crash_count = 0;
    }
    // Other non-crash resets (planned software restarts) keep the count: they
    // happen after mark_stable() would normally have cleared it.

    ESP_LOGI(TAG, "Reset reason %d, consecutive crash boots: %lu", (int)rr, (unsigned long)s_crash_count);

    if (s_config.enable_safe_mode && s_crash_count >= (uint32_t)s_config.bootloop_threshold) {
        ESP_LOGE(TAG, "CRASH LOOP DETECTED (%lu crash boots) — entering safe mode", (unsigned long)s_crash_count);
        s_safe_mode = true;
    }

    if (recovery_mgr_image_pending()) {
        ESP_LOGW(TAG, "Running a new OTA image (pending verify): it will be confirmed after a "
                      "stable period; a crash or reboot before then rolls back to the previous image");
    }
    return ESP_OK;
}

bool recovery_mgr_image_pending(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t recovery_mgr_confirm_image(void)
{
    // Flash write: the caller must have stopped Wi-Fi and the camera first.
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New firmware confirmed — rollback cancelled");
    } else {
        ESP_LOGE(TAG, "Confirming firmware failed: %s", esp_err_to_name(err));
    }
    return err;
}

void recovery_mgr_mark_stable(void)
{
    if (s_crash_count) {
        ESP_LOGI(TAG, "Stable — clearing crash-boot count (%lu)", (unsigned long)s_crash_count);
    }
    s_crash_count = 0;
}

uint32_t recovery_mgr_get_crash_count(void)
{
    return s_crash_count;
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
                s_error_restart_magic = ERROR_RESTART_MAGIC;  // counts as a crash boot
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
