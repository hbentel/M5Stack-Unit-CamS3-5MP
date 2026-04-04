#pragma once
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the log buffer hook.
 *
 * Allocates a 16 KB ring buffer in PSRAM and installs a vprintf hook that
 * tees every ESP_LOGx() line into the ring buffer while still writing to
 * UART. Call as early as possible in app_main() so boot messages are
 * captured.
 *
 * Safe to call multiple times — subsequent calls are no-ops.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if PSRAM allocation failed.
 */
esp_err_t log_buf_init(void);

/**
 * @brief Copy the current log contents into a newly allocated buffer.
 *
 * Reconstructs the ring buffer in chronological order. The caller is
 * responsible for free()-ing the returned pointer.
 *
 * @param[out] out_len  Number of bytes written (excluding null terminator).
 * @return Heap-allocated null-terminated string, or NULL if the buffer is
 *         uninitialised or allocation failed.
 */
char *log_buf_snapshot(size_t *out_len);

#ifdef __cplusplus
}
#endif
