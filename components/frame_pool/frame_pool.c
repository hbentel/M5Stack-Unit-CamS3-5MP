#include "frame_pool.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "frame_pool";

typedef struct {
    frame_buffer_t buffer;
    bool in_use;
} pool_slot_t;

static struct {
    pool_slot_t *slots;
    int count;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t available_sem;
} s_pool = {0};

esp_err_t frame_pool_init(int count, size_t size)
{
    if (s_pool.slots) {
        ESP_LOGW(TAG, "Already initialized!");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing pool with %d buffers of %zu bytes (Total: %zu KB)", 
             count, size, (count * size) / 1024);

    s_pool.mutex = xSemaphoreCreateMutex();
    s_pool.available_sem = xSemaphoreCreateCounting(count, count);
    if (!s_pool.mutex || !s_pool.available_sem) return ESP_ERR_NO_MEM;

    // Allocate the slot metadata array (internal RAM is fine for small structs)
    s_pool.slots = calloc(count, sizeof(pool_slot_t));
    if (!s_pool.slots) return ESP_ERR_NO_MEM;

    s_pool.count = count;

    for (int i = 0; i < count; i++) {
        // Allocate the big buffer in PSRAM
        void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (!ptr) {
            ESP_LOGE(TAG, "Failed to allocate PSRAM buffer %d!", i);
            return ESP_ERR_NO_MEM; // In production, clean up previous allocs
        }
        s_pool.slots[i].buffer.buf = ptr;
        s_pool.slots[i].buffer.capacity = size;
        s_pool.slots[i].buffer.len = 0;
        atomic_init(&s_pool.slots[i].buffer.ref_count, 0);
        s_pool.slots[i].in_use = false;
        s_pool.slots[i].buffer.ctx = (void*)(intptr_t)i; // Store index for debugging
    }
    
    return ESP_OK;
}

frame_buffer_t* frame_pool_get(uint32_t timeout_ms)
{
    if (!s_pool.slots) return NULL;

    if (xSemaphoreTake(s_pool.available_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "Pool exhausted! (Capacity: %d, Timeout: %lu ms)", s_pool.count, (unsigned long)timeout_ms);
        return NULL;
    }

    xSemaphoreTake(s_pool.mutex, portMAX_DELAY);
    
    frame_buffer_t *found = NULL;
    for (int i = 0; i < s_pool.count; i++) {
        if (!s_pool.slots[i].in_use) {
            s_pool.slots[i].in_use = true;
            found = &s_pool.slots[i].buffer;
            atomic_store(&found->ref_count, 1);
            break;
        }
    }
    
    xSemaphoreGive(s_pool.mutex);
    
    if (!found) {
        // Should never happen if counting semaphore is correct, but safe fallback
        xSemaphoreGive(s_pool.available_sem);
        ESP_LOGE(TAG, "Internal error: No free slot despite semaphore token!");
    }
    return found;
}

frame_buffer_t *frame_pool_ref(frame_buffer_t *fb)
{
    if (fb) {
        atomic_fetch_add(&fb->ref_count, 1);
    }
    return fb;
}

static void frame_pool_return_internal(frame_buffer_t *fb)
{
    if (!fb || !s_pool.slots) return;

    xSemaphoreTake(s_pool.mutex, portMAX_DELAY);
    
    // Validate pointer belongs to pool
    bool returned = false;
    for (int i = 0; i < s_pool.count; i++) {
        if (&s_pool.slots[i].buffer == fb) {
            if (s_pool.slots[i].in_use) {
                s_pool.slots[i].in_use = false;
                returned = true;
            } else {
                ESP_LOGE(TAG, "Double free detected for buffer %d!", i);
            }
            break;
        }
    }
    
    if (returned) {
        xSemaphoreGive(s_pool.available_sem);
    } else {
        ESP_LOGE(TAG, "Attempted to return invalid buffer pointer %p", fb);
    }

    xSemaphoreGive(s_pool.mutex);
}

void frame_pool_unref(frame_buffer_t *fb)
{
    if (fb) {
        if (atomic_fetch_sub(&fb->ref_count, 1) == 1) {
            // We were the last holder (count went from 1 to 0)
            frame_pool_return_internal(fb);
        }
    }
}
