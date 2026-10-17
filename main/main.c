#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h" // Required for esp_timer_get_time()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "hw_validate.h"
#include "esp_camera.h"

// Phase 2 Headers
#include "wifi.h"
#include "http_server.h"
#include "frame_pool.h"
#include "mqtt_mgr.h"
#include "recovery_mgr.h" // Added

// Phase 8 Headers
#include "ota_mgr.h"
#include "config_mgr.h"
#include "mdns.h"
#include "log_buf.h"
#include "nvs_flash.h"
#include "esp_wifi.h"

// Default/fallback camera XCLK. The sensor frame rate scales with it (VGA: 10 MHz
// ~9.4 fps, 16 MHz ~15 fps, 20 MHz ~18.8 fps — all error-free since the 2026-10-16
// driver buffer-leak fix). Selectable on /setup (config_mgr); 10 MHz gives the most
// exposure time per frame, and Frigate's detector needs only ~5-6 fps.
#define CAM_XCLK_FREQ_HZ 10000000

// Resolution Configuration: FRAMESIZE_VGA, FRAMESIZE_SVGA, FRAMESIZE_XVGA, FRAMESIZE_UXGA
#define CAM_FRAME_SIZE   FRAMESIZE_VGA

static const char *TAG = "main";

// ========================================
// Phase 1: UnitCamS3 Pin Map
// ========================================
#define CAM_PIN_PWDN    -1 // Tied to GND
#define CAM_PIN_RESET   21
#define CAM_PIN_XCLK    11
#define CAM_PIN_SIOD    17 // SDA
#define CAM_PIN_SIOC    41 // SCL

#define CAM_PIN_D7      13 // Y9
#define CAM_PIN_D6      4  // Y8
#define CAM_PIN_D5      10 // Y7
#define CAM_PIN_D4      5  // Y6
#define CAM_PIN_D3      7  // Y5
#define CAM_PIN_D2      16 // Y4
#define CAM_PIN_D1      15 // Y3
#define CAM_PIN_D0      6  // Y2
#define CAM_PIN_VSYNC   42
#define CAM_PIN_HREF    18
#define CAM_PIN_PCLK    12

// ========================================
// Camera Configuration
// ========================================
static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    .xclk_freq_hz = CAM_XCLK_FREQ_HZ,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = CAM_FRAME_SIZE,
    .fb_count = 4,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST
};

// ========================================
// Camera Image Quality Configuration
// ========================================
// Brightness, contrast, saturation, white balance and effect come from /setup
// (config_mgr_get_image(), PY260 driver levels — 0-based, neutral mid-range, NOT the
// -2..+2 scale of OV sensors). Exposure compensation had no measurable effect and
// stays at the default: 0 = default, 1 = +1, 2 = -1, 3 = +2, 4 = -2, 5 = +3, 6 = -3.
#define CAM_EXPOSURE_COMP   0

// The sensor's own power-on levels (what it falls back to when it resets itself)
#define SENSOR_DEF_BRIGHTNESS 4
#define SENSOR_DEF_CONTRAST   3
#define SENSOR_DEF_SATURATION 3
#define SENSOR_DEF_EFFECT     0
#define SENSOR_DEF_WB_MODE    0   // Auto

// JPEG compression level from /setup: 0 = High, 1 = Default, 2 = Low.
// The PY260 has only these three levels — it ignores camera_config_t.jpeg_quality.
static uint8_t s_cam_quality = 1;

// Applies image settings. `keep` = levels to restore (the sensor's status before a
// camera_reinit, so changes made from Home Assistant survive it); NULL = the /setup
// defaults (config_mgr).
static void apply_camera_settings(const camera_status_t *keep)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
        ESP_LOGE(TAG, "Sensor not found, cannot apply settings");
        return;
    }

    ESP_LOGI(TAG, "Applying camera settings%s...", keep ? " (restoring previous levels)" : "");

    // Read-back verification only here (sensor freshly reset); runtime changes from
    // MQTT are written once, because SCCB reads while streaming destabilize the PY260.
    esp_camera_set_write_verify(true);
    if (s->set_brightness) s->set_brightness(s, keep ? keep->brightness : config_mgr_get_image(CONFIG_IMG_BRIGHTNESS));
    if (s->set_contrast) s->set_contrast(s, keep ? keep->contrast : config_mgr_get_image(CONFIG_IMG_CONTRAST));
    if (s->set_saturation) s->set_saturation(s, keep ? keep->saturation : config_mgr_get_image(CONFIG_IMG_SATURATION));
    if (s->set_special_effect) s->set_special_effect(s, keep ? keep->special_effect : config_mgr_get_image(CONFIG_IMG_EFFECT));
    if (s->set_wb_mode) s->set_wb_mode(s, keep ? keep->wb_mode : config_mgr_get_image(CONFIG_IMG_WB_MODE));
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, keep ? keep->ae_level : CAM_EXPOSURE_COMP);
    if (s->set_quality) s->set_quality(s, s_cam_quality);
    esp_camera_set_write_verify(false);
    // set_whitebal, set_awb_gain, set_gain_ctrl are not implemented by PY260 (wired to set_dummy)

    ESP_LOGI(TAG, "Camera settings applied");

    // Report the applied levels so Home Assistant reflects what the sensor is
    // actually using (no-op if MQTT is disabled or not yet connected).
    mqtt_mgr_publish_camera_state();
}

SemaphoreHandle_t g_camera_reinit_mutex = NULL;

// Current camera XCLK in MHz, for /stats (http_server.c)
int xclk_current_mhz(void)
{
    return camera_config.xclk_freq_hz / 1000000;
}

// Levels to restore on the next camera_reinit() when the camera was deinitialized
// on purpose beforehand (so there is no live sensor status to copy).
static camera_status_t s_pending_restore;
static bool s_pending_restore_valid = false;

// After a reinit the PY260 finishes its own start-up a few seconds later and resets
// some registers to defaults (measured: a restored brightness +1 held ~4 s, then
// dropped to default). This task re-applies the user's levels once, 10 s after a
// reinit, with plain writes (no read-back — SCCB reads while streaming make the
// sensor revert). Skipped when everything is at the defaults.
#define REAPPLY_DELAY_MS 10000
static TaskHandle_t s_reapply_task = NULL;
static camera_status_t s_reapply_levels;

// True if the levels equal what the sensor resets itself to — then its late reset
// changes nothing and no re-apply is needed. (Compared against the SENSOR's power-on
// levels, not the /setup defaults: e.g. WB Office would otherwise silently fall back
// to Auto.)
static bool levels_are_sensor_defaults(const camera_status_t *l)
{
    return l->brightness == SENSOR_DEF_BRIGHTNESS && l->contrast == SENSOR_DEF_CONTRAST &&
           l->saturation == SENSOR_DEF_SATURATION && l->special_effect == SENSOR_DEF_EFFECT &&
           l->wb_mode == SENSOR_DEF_WB_MODE && l->ae_level == CAM_EXPOSURE_COMP;
}

static void reapply_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // A newer reinit restarts the countdown
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(REAPPLY_DELAY_MS)) != 0) {}

        if (xSemaphoreTake(g_camera_reinit_mutex, 0) != pdTRUE) continue;  // reinit running
        sensor_t *s = esp_camera_sensor_get();
        const camera_status_t *l = &s_reapply_levels;
        if (s) {
            ESP_LOGI(TAG, "Re-applying image levels after sensor start-up");
            if (s->set_brightness)     s->set_brightness(s, l->brightness);
            if (s->set_contrast)       s->set_contrast(s, l->contrast);
            if (s->set_saturation)     s->set_saturation(s, l->saturation);
            if (s->set_special_effect) s->set_special_effect(s, l->special_effect);
            if (s->set_wb_mode)        s->set_wb_mode(s, l->wb_mode);
            if (s->set_exposure_ctrl)  s->set_exposure_ctrl(s, l->ae_level);
        }
        xSemaphoreGive(g_camera_reinit_mutex);
    }
}

static void schedule_reapply(const camera_status_t *levels)
{
    if (!levels || levels_are_sensor_defaults(levels)) return;
    if (!s_reapply_task) {
        xTaskCreatePinnedToCore(reapply_task, "cam_reapply", 3072, NULL, 3, &s_reapply_task, 1);
    }
    s_reapply_levels = *levels;
    xTaskNotifyGive(s_reapply_task);
}

// LED wrapper — passed to mqtt_mgr as a callback so mqtt_mgr doesn't need esp_driver_gpio
static void led_set_level(int level)
{
    gpio_set_level(14, level);
}

esp_err_t camera_reinit(void)
{
    if (!g_camera_reinit_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(g_camera_reinit_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Camera reinit already in progress, waiting...");
        xSemaphoreTake(g_camera_reinit_mutex, portMAX_DELAY);
        xSemaphoreGive(g_camera_reinit_mutex);
        return ESP_OK; // Another task just fixed it
    }

    ESP_LOGW(TAG, "Reinitializing camera driver...");
    // Remember the current levels (possibly changed from Home Assistant) to restore.
    camera_status_t prev = {0};
    bool have_prev = false;
    sensor_t *old = esp_camera_sensor_get();
    if (old) {
        prev = old->status;
        have_prev = true;
    } else if (s_pending_restore_valid) {
        prev = s_pending_restore;
        have_prev = true;
    }
    s_pending_restore_valid = false;
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Hardware reset
    gpio_set_level(CAM_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(CAM_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_camera_set_write_verify(true);  // verify the resolution/format writes too
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera reinit failed: 0x%x", err);
        xSemaphoreGive(g_camera_reinit_mutex);
        return err;
    }

    apply_camera_settings(have_prev ? &prev : NULL);
    if (have_prev) schedule_reapply(&prev);

    // Let sensor stabilize
    vTaskDelay(pdMS_TO_TICKS(500));

    // Discard first frame (often corrupt)
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        esp_camera_fb_return(fb);
    }

    ESP_LOGI(TAG, "Camera reinit successful");
    xSemaphoreGive(g_camera_reinit_mutex);
    return ESP_OK;
}

// One-time confirmation of a freshly OTA'd image after it has run stably. This is
// a flash write (otadata), which is only safe with nothing touching cache-mapped
// memory: pause camera users, deinit the camera, stop Wi-Fi, write, then bring
// everything back. Costs Frigate one ~5-10 s gap, once per OTA.
static void confirm_ota_image(void)
{
    ESP_LOGW(TAG, "Confirming new firmware: pausing camera and Wi-Fi for the flash write");
    http_server_pause_camera(true);
    for (int i = 0; i < 100 && !http_server_camera_idle(); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    xSemaphoreTake(g_camera_reinit_mutex, portMAX_DELAY);
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s_pending_restore = s->status;      // camera_reinit() below restores these levels
        s_pending_restore_valid = true;
    }
    esp_camera_deinit();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));        // let the Wi-Fi driver quiesce (as in ota_mgr)

    recovery_mgr_confirm_image();

    esp_wifi_start();                       // STA_START handler reconnects
    xSemaphoreGive(g_camera_reinit_mutex);

    camera_reinit();
    http_server_pause_camera(false);
}

void app_main(void)
{
    g_camera_reinit_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  UnitCamS3-5MP Custom Firmware v2");
    ESP_LOGI(TAG, "  Boot Sequence Started");
    ESP_LOGI(TAG, "========================================");

    /* Phase 0: Validate PSRAM */
    esp_err_t err = hw_validate_psram();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PSRAM validation FAILED. Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    log_buf_init(); // Start capturing logs to PSRAM ring buffer (non-fatal if PSRAM unavailable)

    // Initialize NVS here, before recovery_mgr_init() and config_mgr_init() read
    // it. Previously the only nvs_flash_init() lived inside wifi_init_sta(), which
    // runs AFTER recovery_mgr_init() — so recovery's nvs_open() failed every boot,
    // the health timer never armed, and esp_ota_mark_app_valid_cancel_rollback()
    // was never called → an OTA'd image would roll back on the next reboot. This
    // is a flash READ/erase only and runs before the camera starts, so it is safe.
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (err=0x%x) — erasing and reinitializing", err);
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: 0x%x — recovery/config NVS will fall back to defaults", err);
    }

    // Commit a /setup save or factory reset staged in RTC RAM by the previous
    // boot. /setup runs with Wi-Fi up, where flash writes are unsafe, so the
    // NVS write is deferred to this pre-Wi-Fi window.
    config_mgr_apply_staged();

    // GPIO 14 — onboard LED, active high, controlled via MQTT unitcams3/led/set
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << 14),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(14, 0); // LED off at boot

    ESP_LOGI(TAG, "--- Initializing Frame Pool ---");
    // 8 buffers x 512KB = 4.0MB fixed PSRAM usage
    // Sized to support up to UXGA (1600x1200) high-quality JPEGs
    // 5 slots × 512 KB = 2.5 MB PSRAM.
    // Normal peak usage is 2 slots (s_broadcast_fb + 1 worker mid-send, all
    // workers share the same ref). A simultaneous snapshot (GET /) holds a ref
    // to the current broadcast frame while sending the HTTP response; combined
    // with a worker mid-send on the previous frame, this can exhaust a 3-slot
    // pool and stall the broadcaster for up to ~5s → Frigate 20s watchdog fires.
    // 5 slots eliminates the exhaustion: snapshot(1) + worker(1) + broadcaster(1)
    // still leaves 2 free slots for the broadcaster to cycle into.
    err = frame_pool_init(5, 512 * 1024);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Frame Pool Init Failed!");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* Phase 2: Network & Services */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Phase 2: Network & Services");
    ESP_LOGI(TAG, "========================================");

    // Recovery manager: counts crash boots in RTC memory (no flash write) and decides
    // safe mode. A new OTA image is confirmed later, after a stable period (see the
    // supervision loop at the end of app_main).
    ESP_LOGI(TAG, "--- Starting Recovery Manager ---");
    err = recovery_mgr_init(NULL); // Use defaults
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Recovery Mgr Init Failed!");
    }

    ESP_LOGI(TAG, "--- Starting Wi-Fi ---");
    err = wifi_init_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed! Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // Check for pending OTA *before* the camera is initialized.
    // If a URL was saved to RTC RAM by the previous boot (via MQTT trigger),
    // download + flash it now and reboot into the new firmware.
    // Camera DMA is never started, so flash operations cannot race with it.
    ESP_LOGI(TAG, "--- Checking for pending OTA ---");
    ota_mgr_run_pending(); // returns only if no pending OTA (or OTA failed)

    ESP_LOGI(TAG, "--- Loading Config ---");
    config_mgr_init();

    ESP_LOGI(TAG, "--- Starting mDNS ---");
    mdns_init();
    mdns_hostname_set(config_mgr_get_device_id());
    mdns_instance_name_set("M5Stack UnitCamS3-5MP");
    mdns_service_add("HTTP", "_http", "_tcp", 80, NULL, 0);
    mdns_service_add("Stream", "_http", "_tcp", 81, NULL, 0);
    ESP_LOGI(TAG, "mDNS hostname: %s.local", config_mgr_get_device_id());

    ESP_LOGI(TAG, "--- Starting MQTT Manager ---");
    if (config_mgr_is_mqtt_enabled()) {
        err = mqtt_mgr_start(
            config_mgr_get_mqtt_url(),
            config_mgr_get_mqtt_user(),
            config_mgr_get_mqtt_pass(),
            config_mgr_get_device_id());
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MQTT Init Failed! Continuing...");
        }
        mqtt_mgr_register_reprovision_callback(wifi_start_reprovision);
        mqtt_mgr_register_led_callback(led_set_level);
    } else {
        ESP_LOGI(TAG, "MQTT disabled by config — skipping");
    }

    // Register the MQTT OTA handler before the camera starts, so a firmware update
    // can still be delivered remotely if camera init fails or hangs.
    ESP_LOGI(TAG, "--- Initializing OTA Manager ---");
    err = ota_mgr_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA Manager init failed! Continuing...");
    }

    /* Phase 1: Initialize Camera AFTER Wi-Fi/NVS are settled.
     * esp_camera_init() enables the VSYNC ISR (cam_start=1). If it runs
     * concurrently with Wi-Fi's NVS flash reads (which disable the OPI cache
     * bus), the ISR touching GDMA/LCD_CAM registers causes a double exception.
     * Initializing here guarantees the cache is stable before the ISR goes live.
     */
    ESP_LOGI(TAG, "--- Initializing Camera Driver ---");
    camera_config.frame_size = config_mgr_get_cam_resolution();
    camera_config.xclk_freq_hz = config_mgr_get_xclk_mhz() * 1000000;
    if (camera_config.xclk_freq_hz != CAM_XCLK_FREQ_HZ && recovery_mgr_get_crash_count() > 0) {
        // The previous boot crashed: don't risk the faster clock again until stable.
        ESP_LOGW(TAG, "Previous boot crashed — using %d MHz XCLK this boot instead of %d MHz",
                 CAM_XCLK_FREQ_HZ / 1000000, camera_config.xclk_freq_hz / 1000000);
        camera_config.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
    }
    s_cam_quality            = config_mgr_get_cam_quality();
    bool safe_mode = recovery_mgr_is_safe_mode();
    if (safe_mode) {
        // Repeated crash boots: the camera is the most likely culprit. Leave it off so
        // HTTP (logs, coredump), MQTT and OTA stay up for a remote fix.
        ESP_LOGE(TAG, "SAFE MODE — camera not started (power-cycle or OTA to recover)");
        err = ESP_FAIL;
    } else {
        err = esp_camera_init(&camera_config);
    }
    // Don't halt on failure: HTTP (logs, /health), MQTT restart and OTA must stay
    // reachable so a dead sensor can be diagnosed and fixed remotely. The camera is
    // retried after the servers are up.
    bool camera_ok = (err == ESP_OK);
    if (!camera_ok) {
        ESP_LOGE(TAG, "Camera Init Failed! Error: 0x%x — continuing without camera, will retry", err);
    } else {
        apply_camera_settings(NULL);
        // The sensor's late start-up reset can clobber these too (WB Office once read
        // back Auto after boot) — re-apply once after it has settled.
        sensor_t *s0 = esp_camera_sensor_get();
        if (s0) schedule_reapply(&s0->status);
    }

    // Prime DMA pipeline — Wi-Fi is fully connected, OPI bus is stable
    if (camera_ok) ESP_LOGI(TAG, "--- Priming camera pipeline ---");
    for (int attempt = 0; camera_ok && attempt < 4; attempt++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            ESP_LOGI(TAG, "Warmup frame captured (%zu bytes, attempt %d)", fb->len, attempt + 1);
            esp_camera_fb_return(fb);
            break;
        }
        ESP_LOGW(TAG, "Warmup capture failed (attempt %d), reinitializing...", attempt + 1);
        camera_reinit();
    }

#ifdef CONFIG_UNITCAMS3_FPS_TEST_MODE
    ESP_LOGI(TAG, "--- ENTERING FPS TEST MODE ---");
    ESP_LOGW(TAG, "HTTP Servers will NOT start.");

    int64_t last_time = esp_timer_get_time();
    int frame_count = 0;
    size_t total_bytes = 0;
    cam_stats_t last_stats = {0};

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            frame_count++;
            total_bytes += fb->len;
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGW(TAG, "FPS Test: Frame drop / timeout");
        }

        int64_t now = esp_timer_get_time();
        if ((now - last_time) >= 5000000) { // Every 5 seconds
            float elapsed = (now - last_time) / 1000000.0f;
            float fps = frame_count / elapsed;
            float kbps = (total_bytes / 1024.0f) / elapsed;

            cam_stats_t cur = {0};
            esp_camera_get_stats(&cur);
            uint32_t d_vsync  = cur.vsync_isr_count       - last_stats.vsync_isr_count;
            uint32_t d_start  = cur.start_trigger_count    - last_stats.start_trigger_count;
            uint32_t d_eof    = cur.eof_count              - last_stats.eof_count;
            uint32_t d_nosoi  = cur.no_soi_count           - last_stats.no_soi_count;
            uint32_t d_noeoi  = cur.no_eoi_count           - last_stats.no_eoi_count;
            uint32_t d_drops  = cur.drops_no_free_buf      - last_stats.drops_no_free_buf;
            uint32_t d_qovf   = cur.queue_overflow_count   - last_stats.queue_overflow_count;

            uint32_t d_partial = cur.partial_chunk_bytes - last_stats.partial_chunk_bytes;
            ESP_LOGI(TAG, "FPS: %.2f | %.1f KB/s | frames=%d", fps, kbps, frame_count);
            ESP_LOGI(TAG, "  VSYNC=%lu START=%lu EOF=%lu | NO-SOI=%lu NO-EOI=%lu | drops=%lu qovf=%lu | partial_bytes=%lu",
                     (unsigned long)d_vsync, (unsigned long)d_start, (unsigned long)d_eof,
                     (unsigned long)d_nosoi, (unsigned long)d_noeoi,
                     (unsigned long)d_drops, (unsigned long)d_qovf,
                     (unsigned long)d_partial);

            last_stats = cur;
            last_time = now;
            frame_count = 0;
            total_bytes = 0;
        }

        vTaskDelay(1);
    }
#endif

    ESP_LOGI(TAG, "--- Starting HTTP Server (Port 80) ---");
    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP Server failed to start! Halting.");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "--- Starting Stream Server (Port 81) ---");
    err = start_stream_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream Server failed to start! Continuing...");
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  SYSTEM READY%s", camera_ok ? "" : " (CAMERA FAILED — retrying)");
    ESP_LOGI(TAG, "  HTTP: port 80  |  Stream: port 81");
    ESP_LOGI(TAG, "========================================");

    // Post-boot supervision (this task):
    //  - camera failed at boot (not safe mode): retry every 30 s. A sensor whose
    //    control bus is hung may need a power cycle; the error stays in /api/logs.
    //  - after STABLE_AFTER_S of uptime: clear the crash-boot count and, if this is
    //    a new OTA image, confirm it (until then a crash or reboot rolls back).
    #define STABLE_AFTER_S 120
    bool stable = false;
    for (int t = 1; !stable || (!camera_ok && !safe_mode); t++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!camera_ok && !safe_mode && t % 30 == 0) {
            ESP_LOGW(TAG, "Camera init retry %d...", t / 30);
            camera_ok = (camera_reinit() == ESP_OK);
        }
        if (!stable && t >= STABLE_AFTER_S) {
            stable = true;
            recovery_mgr_mark_stable();
            if (recovery_mgr_image_pending()) {
                if (camera_ok) {
                    confirm_ota_image();
                } else {
                    // No camera to pause; Wi-Fi still must be stopped for the write.
                    esp_wifi_stop();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    recovery_mgr_confirm_image();
                    esp_wifi_start();
                }
            }
        }
    }
}
