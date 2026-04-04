#include "log_buf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "log_buf";

#define LOG_BUF_SIZE  (16 * 1024)
#define LINE_BUF_SIZE 512   // max formatted line length captured per call

static char            *s_buf      = NULL;
static size_t           s_pos      = 0;
static bool             s_wrapped  = false;
static portMUX_TYPE     s_mux      = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t   s_orig_vprintf = NULL;

/* vprintf hook — installed via esp_log_set_vprintf().
 * Called for every ESP_LOGx() line from any task or ISR.
 * Must always write to UART and must never crash even if the ring
 * buffer is not yet initialised. */
static int log_hook(const char *fmt, va_list args)
{
    /* Copy args BEFORE consuming them — va_list is single-use. */
    va_list args_copy;
    va_copy(args_copy, args);

    /* Always write to UART via the original handler. */
    int ret = s_orig_vprintf(fmt, args);

    /* If buffer not ready, nothing more to do. */
    if (!s_buf) {
        va_end(args_copy);
        return ret;
    }

    /* Format the line into a stack buffer outside the critical section. */
    char tmp[LINE_BUF_SIZE];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args_copy);
    va_end(args_copy);

    if (n <= 0) return ret;
    /* vsnprintf returns the number of chars that WOULD be written;
     * cap at what actually fits in tmp (line is truncated at LINE_BUF_SIZE-1). */
    if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;

    /* Append to ring buffer under spinlock (ISR-safe). */
    portENTER_CRITICAL_SAFE(&s_mux);
    for (int i = 0; i < n; i++) {
        s_buf[s_pos++] = tmp[i];
        if (s_pos >= LOG_BUF_SIZE) {
            s_pos     = 0;
            s_wrapped = true;
        }
    }
    portEXIT_CRITICAL_SAFE(&s_mux);

    return ret;
}

esp_err_t log_buf_init(void)
{
    if (s_buf) return ESP_OK;   /* already initialised */

    s_buf = heap_caps_malloc(LOG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        ESP_LOGW(TAG, "PSRAM allocation failed — log buffer disabled");
        return ESP_ERR_NO_MEM;
    }

    s_orig_vprintf = esp_log_set_vprintf(log_hook);
    ESP_LOGI(TAG, "Log buffer ready (%d KB, PSRAM)", LOG_BUF_SIZE / 1024);
    return ESP_OK;
}

char *log_buf_snapshot(size_t *out_len)
{
    *out_len = 0;
    if (!s_buf) return NULL;

    /* Snapshot position state under lock — keeps critical section tiny. */
    portENTER_CRITICAL_SAFE(&s_mux);
    size_t pos     = s_pos;
    bool   wrapped = s_wrapped;
    portEXIT_CRITICAL_SAFE(&s_mux);

    size_t len = wrapped ? LOG_BUF_SIZE : pos;
    if (len == 0) {
        /* Return an empty but valid string. */
        char *empty = malloc(1);
        if (empty) *empty = '\0';
        return empty;
    }

    char *snap = malloc(len + 1);
    if (!snap) return NULL;

    if (!wrapped) {
        /* Buffer has not wrapped: data is s_buf[0..pos). */
        memcpy(snap, s_buf, len);
    } else {
        /* Buffer has wrapped: oldest data starts at pos, wraps to 0.
         * Reconstruct in chronological order. */
        size_t tail = LOG_BUF_SIZE - pos;   /* bytes from pos to end */
        memcpy(snap,        s_buf + pos, tail);
        memcpy(snap + tail, s_buf,       pos);
    }
    snap[len] = '\0';
    *out_len  = len;
    return snap;
}
