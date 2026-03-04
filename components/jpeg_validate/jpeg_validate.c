#include "jpeg_validate.h"
#include "esp_log.h"
#include <stdatomic.h>

static const char *TAG = "jpeg_validate";
static atomic_uint_fast32_t s_drop_count = 0;

bool jpeg_validate_frame(const camera_fb_t *fb)
{
    if (!fb || !fb->buf) {
        ESP_LOGW(TAG, "NULL frame buffer");
        s_drop_count++;
        return false;
    }

    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGW(TAG, "Not JPEG format (%d)", fb->format);
        s_drop_count++;
        return false;
    }

    // Minimum sane JPEG: SOI(2) + at least one marker + EOI(2)
    if (fb->len < 100) {
        ESP_LOGW(TAG, "Frame too small (%zu bytes)", fb->len);
        s_drop_count++;
        return false;
    }

    // Check SOI marker (0xFF 0xD8)
    if (fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) {
        ESP_LOGW(TAG, "Missing SOI marker (got 0x%02X 0x%02X)", fb->buf[0], fb->buf[1]);
        s_drop_count++;
        return false;
    }

    // Check EOI marker (0xFF 0xD9) at end of data
    if (fb->buf[fb->len - 2] != 0xFF || fb->buf[fb->len - 1] != 0xD9) {
        ESP_LOGW(TAG, "Missing EOI marker at offset %zu (got 0x%02X 0x%02X)",
                 fb->len - 2, fb->buf[fb->len - 2], fb->buf[fb->len - 1]);
        s_drop_count++;
        return false;
    }

    return true;
}

uint32_t jpeg_validate_get_drop_count(void)
{
    return s_drop_count;
}
