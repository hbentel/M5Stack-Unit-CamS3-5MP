#include "ota_mgr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "mqtt_mgr.h"
#include "recovery_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_heap_caps.h"
#include <inttypes.h>

static const char *TAG = "ota_mgr";

// 4KB fits in internal RAM (SPIRAM_MALLOC_ALWAYSINTERNAL=16384).
// esp_ota_write reads from this buffer while flash+PSRAM cache is disabled —
// a PSRAM-sourced buffer would cause an instruction fetch / cache-disable crash.
#define OTA_BUF_SIZE 4096

// ESP32 firmware image magic byte — first byte of every valid .bin
#define ESP_IMAGE_MAGIC 0xE9

// ========================================
// RTC RAM storage for pending OTA URL.
//
// WHY RTC RAM instead of NVS:
// nvs_commit() erases+writes flash, which disables the OPI PSRAM cache.
// If the camera DMA is running at that moment, a PSRAM access during the
// cache-disable window causes an ExcCause=7 (cache-disabled cached-memory
// access) → double exception → device crash.
//
// RTC_DATA_ATTR places data in RTC Fast RAM — a plain SRAM write, no flash,
// no cache disable, safe while camera DMA is running.
// RTC RAM is retained across software resets (esp_restart) so the URL
// survives the reboot triggered by ota_mgr_start_url().
//
// Two magic values are stored for corruption detection. A URL length field
// provides a third check. The probability of all three matching by accident
// (stale RTC state from a previous crash) is negligible.
// ========================================
#define OTA_MAGIC_A  0xDE0CA001UL  // arbitrary canary A
#define OTA_MAGIC_B  0xA57EF002UL  // arbitrary canary B

// RTC_NOINIT_ATTR (section ".rtc_noinit", NOLOAD) — NOT reloaded from flash on
// software reset (esp_restart). The bootloader skips NOLOAD sections, so the
// values written by rtc_save_url() survive the reboot triggered by ota_mgr_start_url().
//
// RTC_DATA_ATTR (section ".rtc.data") is WRONG for this use case: the bootloader
// re-copies .rtc.data from the flash image on every non-deep-sleep reset
// (esp_image_format.c: load_rtc_memory = reset_reason != DEEP_SLEEP), overwriting
// whatever was written to RTC RAM before esp_restart().
// RTC_DATA_ATTR is only suitable for deep-sleep persistence, not software-reset persistence.
static RTC_NOINIT_ATTR uint32_t s_magic_a;
static RTC_NOINIT_ATTR uint32_t s_magic_b;
static RTC_NOINIT_ATTR uint32_t s_url_len;
static RTC_NOINIT_ATTR char     s_url[256];

static bool rtc_has_pending_url(void)
{
    return (s_magic_a == OTA_MAGIC_A &&
            s_magic_b == OTA_MAGIC_B &&
            s_url_len > 0 &&
            s_url_len < sizeof(s_url) &&
            strnlen(s_url, sizeof(s_url)) == s_url_len);
}

static void rtc_save_url(const char *url)
{
    size_t len = strlen(url);
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    s_url_len = (uint32_t)len;
    s_magic_a = OTA_MAGIC_A;
    s_magic_b = OTA_MAGIC_B;
}

static void rtc_clear_url(void)
{
    s_magic_a = 0;
    s_magic_b = 0;
    s_url_len = 0;
    memset(s_url, 0, sizeof(s_url));
}

// ========================================
// Core OTA: download-to-PSRAM → stop-wifi → flash
//
// WHY this two-phase approach:
// esp_ota_begin/write disable the OPI PSRAM cache for the duration of each
// flash erase/write operation. On IDF v6, Wi-Fi ISRs are marked
// ESP_INTR_FLAG_IRAM (so they survive esp_intr_noniram_disable) but their
// handlers call into lwIP code which is in flash-cached memory. If a Wi-Fi
// or TCP receive ISR fires during the cache-disable window it crashes with
// ExcCause=7 → double exception.
//
// Fix: download the complete firmware into a PSRAM buffer first (no flash
// writes during download), close the HTTP connection, stop Wi-Fi (removes
// all active ISRs), then flash using a 4KB internal-SRAM staging buffer.
// During esp_ota_write(), the only memory accessed is internal SRAM — no
// PSRAM, no flash-cached code, no Wi-Fi ISRs → safe on both v5 and v6.
//
// Precondition: camera is NOT running (OTA runs before esp_camera_init()).
// ========================================

// Maximum firmware binary we will accept (must fit in free PSRAM at OTA time).
// frame_pool (4×512KB) is already allocated before OTA runs, leaving ~6MB.
#define OTA_MAX_FW_SIZE (4 * 1024 * 1024)  // 4 MB ceiling

static esp_err_t run_ota_from_url(const char *url)
{
    esp_err_t err = ESP_FAIL;
    uint8_t *fw_buf  = NULL;  // PSRAM: full firmware image
    uint8_t *wr_buf  = NULL;  // internal SRAM: 4KB staging for esp_ota_write
    esp_http_client_handle_t client = NULL;
    esp_ota_handle_t ota_handle = 0;
    bool ota_begun   = false;
    bool wifi_stopped = false;

    // Internal SRAM staging buffer — must NOT be PSRAM: during esp_ota_write()
    // the OPI PSRAM cache is disabled and any PSRAM access would crash.
    wr_buf = heap_caps_malloc(OTA_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!wr_buf) {
        ESP_LOGE(TAG, "Failed to alloc %d byte write buffer (internal SRAM)", OTA_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_cfg = {
        .url        = url,
        .timeout_ms = 30000,
    };
    client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        goto cleanup;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int http_status    = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d, content-length: %d bytes", http_status, content_length);

    if (http_status != 200) {
        ESP_LOGE(TAG, "HTTP error %d — aborting (not flashing)", http_status);
        err = ESP_FAIL;
        goto cleanup;
    }

    if (content_length <= 0 || content_length > OTA_MAX_FW_SIZE) {
        ESP_LOGE(TAG, "Implausible content-length %d — aborting", content_length);
        err = ESP_FAIL;
        goto cleanup;
    }

    // Phase 1: Download complete firmware into PSRAM.
    // Accessing PSRAM here is safe — no flash writes in progress.
    fw_buf = heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fw_buf) {
        ESP_LOGE(TAG, "Failed to alloc %d bytes PSRAM for firmware", content_length);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Firmware buffer: %d bytes at %p (PSRAM)", content_length, fw_buf);

    int fw_len = 0;
    while (fw_len < content_length) {
        esp_task_wdt_reset();
        int want = content_length - fw_len;
        if (want > OTA_BUF_SIZE) want = OTA_BUF_SIZE;
        int got = esp_http_client_read(client, (char *)(fw_buf + fw_len), want);
        if (got < 0) {
            ESP_LOGE(TAG, "HTTP read error (%d)", got);
            err = ESP_FAIL;
            goto cleanup;
        }
        if (got == 0) break;
        fw_len += got;
        if (fw_len % (200 * 1024) < OTA_BUF_SIZE) {
            ESP_LOGI(TAG, "Download: %d / %d bytes", fw_len, content_length);
        }
    }

    if (fw_len != content_length) {
        ESP_LOGE(TAG, "Download truncated: got %d, expected %d — aborting",
                 fw_len, content_length);
        err = ESP_FAIL;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Download complete: %d bytes", fw_len);

    // Validate firmware magic byte before touching the OTA partition.
    if (fw_buf[0] != ESP_IMAGE_MAGIC) {
        ESP_LOGE(TAG, "Bad firmware magic 0x%02x (expected 0xE9) — not a valid .bin",
                 fw_buf[0]);
        err = ESP_FAIL;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Firmware magic byte OK (0xE9)");

    // Phase 2: Close HTTP and stop Wi-Fi BEFORE the first flash write.
    // esp_ota_begin/write disable the OPI PSRAM cache. If any Wi-Fi or lwIP
    // ISR fires during the cache-disable window and touches PSRAM, it causes
    // ExcCause=7 → double exception. Stopping Wi-Fi removes all active ISRs.
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;

    ESP_LOGI(TAG, "Stopping Wi-Fi before flash write (cache-disable safety)...");
    esp_wifi_stop();
    wifi_stopped = true;
    vTaskDelay(pdMS_TO_TICKS(100)); // let the driver fully quiesce

    // Phase 3: Flash.
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "No OTA update partition found");
        err = ESP_FAIL;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Target: '%s' at 0x%" PRIx32, part->label, part->address);

    err = esp_ota_begin(part, fw_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    ota_begun = true;

    // Copy PSRAM → internal SRAM (safe: no flash write yet), then write to flash.
    // During esp_ota_write() the PSRAM cache is disabled; wr_buf (internal SRAM)
    // is the only memory accessed — safe by design.
    int written = 0;
    while (written < fw_len) {
        esp_task_wdt_reset();
        int chunk = fw_len - written;
        if (chunk > OTA_BUF_SIZE) chunk = OTA_BUF_SIZE;
        memcpy(wr_buf, fw_buf + written, chunk);        // PSRAM → SRAM (no flash op)
        err = esp_ota_write(ota_handle, wr_buf, chunk); // SRAM → flash (cache off)
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        written += chunk;
        if (written % (200 * 1024) < OTA_BUF_SIZE) {
            ESP_LOGI(TAG, "Flash: %d / %d bytes", written, fw_len);
        }
    }

    err = esp_ota_end(ota_handle);
    ota_begun = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA flash complete — %d bytes written to '%s'", written, part->label);

cleanup:
    if (ota_begun)    esp_ota_abort(ota_handle);
    if (client)       esp_http_client_cleanup(client);
    if (fw_buf)       heap_caps_free(fw_buf);
    if (wr_buf)       heap_caps_free(wr_buf);
    if (wifi_stopped && err != ESP_OK) {
        // Flash failed after Wi-Fi was stopped — reboot cleanly rather than
        // trying to resume normal boot with a half-initialized network stack.
        // RTC RAM is already cleared so no OTA boot loop will occur.
        ESP_LOGE(TAG, "OTA failed after Wi-Fi stop — rebooting");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    return err;
}

// ========================================
// Public API: ota_mgr_run_pending
//
// Called from main() AFTER wifi connects, BEFORE esp_camera_init().
// If a pending URL is in RTC RAM: run OTA, then esp_restart().
// If OTA fails: clear RTC RAM (prevent boot loop), log, return to normal boot.
// If no pending URL: return ESP_OK immediately (normal boot continues).
// ========================================

esp_err_t ota_mgr_run_pending(void)
{
    if (!rtc_has_pending_url()) {
        return ESP_OK; // No pending OTA — normal boot
    }

    // Copy URL out of RTC RAM before clearing it.
    char url[256];
    strncpy(url, s_url, sizeof(url));
    url[sizeof(url) - 1] = '\0';

    // Clear RTC RAM *before* attempting OTA.
    // If OTA fails or the device crashes mid-flash, the next boot will
    // not retry the same bad URL and get stuck in an OTA boot loop.
    rtc_clear_url();

    ESP_LOGW(TAG, "========================================");
    ESP_LOGW(TAG, "  PENDING OTA DETECTED");
    ESP_LOGW(TAG, "  URL: %s", url);
    ESP_LOGW(TAG, "  Camera NOT initialized — safe to flash");
    ESP_LOGW(TAG, "========================================");

    // Register main task with WDT — download can take 30-60s
    esp_task_wdt_add(NULL);

    esp_err_t err = run_ota_from_url(url);

    esp_task_wdt_delete(NULL);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA complete — rebooting to new firmware");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        /* NOT REACHED */
    }

    // OTA failed. RTC RAM is already cleared. Log the error and fall through
    // to normal boot — all services (camera, HTTP, MQTT) will start normally.
    // The user can retrigger OTA via MQTT once the device is back online.
    ESP_LOGE(TAG, "OTA failed (%s) — resuming normal boot", esp_err_to_name(err));
    return ESP_FAIL;
}

// ========================================
// Public API: ota_mgr_start_url
//
// Called from MQTT when unitcams3/ota/set is received.
// Stores URL in RTC RAM (pure memory write — no flash, no cache disable,
// safe while camera DMA is running), publishes status, then reboots.
// On the next boot, ota_mgr_run_pending() picks up the URL from RTC RAM
// and runs the OTA before the camera is ever initialized.
// ========================================

esp_err_t ota_mgr_start_url(const char *url)
{
    if (!url || strlen(url) == 0 || strlen(url) >= sizeof(s_url)) {
        ESP_LOGE(TAG, "Invalid OTA URL (empty or too long)");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(TAG, "OTA requested — saving URL to RTC RAM and rebooting");
    ESP_LOGW(TAG, "URL: %s", url);

    // Write URL to RTC RAM — a plain SRAM store, no flash involved.
    // Safe to call from any task even while camera DMA is active.
    rtc_save_url(url);

    // Brief delay so the MQTT broker sees the publish before we disconnect.
    mqtt_mgr_publish("ota_status", "pending_reboot");
    vTaskDelay(pdMS_TO_TICKS(300));

    recovery_mgr_signal_planned_reboot();

    ESP_LOGI(TAG, "Rebooting now for OTA...");
    esp_restart();
    /* NOT REACHED */
    return ESP_OK;
}

// ========================================
// Init — registers MQTT OTA callback
// ========================================

esp_err_t ota_mgr_init(void)
{
    mqtt_mgr_register_ota_callback(ota_mgr_start_url);

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "OTA manager ready. Running from: %s", running->label);

    return ESP_OK;
}
