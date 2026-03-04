#pragma once

#include <stddef.h>
#include <stdint.h>
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
 * 
 * @return Pointer to a frame_buffer_t, or NULL if no buffers are available.
 */
frame_buffer_t* frame_pool_get(void);

/**
 * @brief Return a buffer to the pool, marking it as available.
 * 
 * @param fb Pointer to the frame_buffer_t to return.
 */
void frame_pool_return(frame_buffer_t *fb);

#ifdef __cplusplus
}
#endif
