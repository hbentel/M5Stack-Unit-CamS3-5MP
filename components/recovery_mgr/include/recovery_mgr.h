#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

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
 * Counts consecutive crash boots in RTC memory (no flash write) and enters safe
 * mode at the threshold. Does NOT confirm a new OTA image — see
 * recovery_mgr_confirm_image().
 */
esp_err_t recovery_mgr_init(const recovery_config_t *config);

/**
 * @brief Report an error to the manager.
 * The manager tracks error counts and triggers escalation (reinit, reboot, etc).
 */
void recovery_mgr_report_error(recovery_error_t error);

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

/** @brief True if the running image is a new OTA image awaiting confirmation. */
bool recovery_mgr_image_pending(void);

/**
 * @brief Confirm the running OTA image (cancels rollback). This is a FLASH WRITE:
 *        the caller must stop Wi-Fi and the camera first.
 */
esp_err_t recovery_mgr_confirm_image(void);

/** @brief Mark the system stable: clears the consecutive crash-boot count. */
void recovery_mgr_mark_stable(void);

/** @brief Consecutive crash boots counted so far (RTC memory). */
uint32_t recovery_mgr_get_crash_count(void);

#ifdef __cplusplus
}
#endif
