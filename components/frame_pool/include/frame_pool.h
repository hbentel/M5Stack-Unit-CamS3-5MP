#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Structure representing a buffer from the pool.
 */
typedef struct {
    uint8_t *buf;       // Pointer to the pre-allocated PSRAM buffer
    size_t len;         // Length of valid data currently stored
    size_t capacity;    // Maximum capacity of this buffer
    int64_t timestamp;  // Capture timestamp (us)
    _Atomic uint32_t ref_count; // Atomic reference count
    void *ctx;          // Internal context for the pool manager
} frame_buffer_t;

/**
 * @brief Initialize the frame pool with pre-allocated PSRAM buffers.
 * 
 * @param count Number of buffers to allocate (e.g., 2 or 3)
 * @param size Size of each buffer in bytes (e.g., 256KB for VGA JPEG)
 * @return ESP_OK on success
 */
esp_err_t frame_pool_init(int count, size_t size);

/**
 * @brief Get a free buffer from the pool.
 * @param timeout_ms Wait time in milliseconds before failing
 * @return Pointer to buffer, or NULL if pool is exhausted or timeout occurs
 *         The returned buffer has ref_count initialized to 1.
 */
frame_buffer_t* frame_pool_get(uint32_t timeout_ms);

/**
 * @brief Increment the reference count of a buffer.
 * @param fb Pointer to buffer
 * @return The same pointer (for convenience)
 */
frame_buffer_t *frame_pool_ref(frame_buffer_t *fb);

/**
 * @brief Decrement the reference count. If it reaches 0, the buffer is returned to the pool.
 * @param fb Pointer to buffer
 */
void frame_pool_unref(frame_buffer_t *fb);

#ifdef __cplusplus
}
#endif
