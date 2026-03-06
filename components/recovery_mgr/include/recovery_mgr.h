#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RECOVERY_ERR_NONE = 0,
    RECOVERY_ERR_CAMERA_INIT,
    RECOVERY_ERR_FRAME_TIMEOUT,
    RECOVERY_ERR_STREAM_SEND,
    RECOVERY_ERR_WIFI_DISCONNECT,
    RECOVERY_ERR_I2C_TIMEOUT
} recovery_error_t;

typedef struct {
    bool enable_reboot;
    bool enable_safe_mode;
    int max_reinit_attempts; // Before hardware reset
    int bootloop_threshold;  // Reboots in X mins to trigger safe mode
} recovery_config_t;

/**
 * @brief Initialize the Recovery Manager.
 * Checks NVS for boot loops and sets safe mode if needed.
 */
esp_err_t recovery_mgr_init(const recovery_config_t *config);

/**
 * @brief Report an error to the manager.
 * The manager tracks error counts and triggers escalation (reinit, reboot, etc).
 */
void recovery_mgr_report_error(recovery_error_t error);

/**
 * @brief Mark the system as healthy, resetting the boot counter.
 * Called automatically via timer after 2 minutes of stable uptime.
 * Can also be called manually.
 */
void recovery_mgr_mark_healthy(void);

/**
 * @brief Check if the device is currently in Safe Mode.
 */
bool recovery_mgr_is_safe_mode(void);

/**
 * @brief Get the total number of recovery errors reported since boot.
 */
int recovery_mgr_get_error_count(void);

/**
 * @brief Signal that the next reboot is intentional (OTA, restart command).
 * Prevents the boot counter from incrementing on the subsequent boot,
 * avoiding false boot-loop detection after deliberate reboots.
 * Must be called before esp_restart().
 */
void recovery_mgr_signal_planned_reboot(void);

#ifdef __cplusplus
}
#endif
