#include "http_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "jpeg_validate.h"
#include "frame_pool.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_task_wdt.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "config_mgr.h"
#include "recovery_mgr.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "http_server";

static httpd_handle_t s_server = NULL;
static httpd_handle_t s_stream_server = NULL;
static volatile bool s_ota_pending = false;

#define MJPEG_BOUNDARY "frame"
#define STREAM_PART_HDR "--" MJPEG_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n"
#define STREAM_FRAME_INTERVAL_MS 250  // ~4 FPS target to prevent Wi-Fi saturation

// Forward declaration - defined in main.c
extern esp_err_t camera_reinit(void);

// Boot timestamp for uptime calculation
static int64_t s_boot_time_us;

// Handler for the "/" endpoint — returns a single JPEG snapshot
static esp_err_t capture_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Received request for JPEG frame...");

    // 1. Acquire a buffer from the pool first
    //    If pool is empty, we are too busy to serve.
    frame_buffer_t *pool_fb = frame_pool_get();
    if (!pool_fb) {
        ESP_LOGE(TAG, "Frame pool exhausted! Dropping request.");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 2. Grab a fresh frame (CAMERA_GRAB_LATEST ensures it's recent)
    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
        ESP_LOGW(TAG, "Capture failed, reinitializing camera...");
        esp_err_t err = camera_reinit();
        if (err == ESP_OK) {
            fb = esp_camera_fb_get();
        }
    }

    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed even after reinit");
        frame_pool_return(pool_fb); // Release pool buffer
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 3. Validate JPEG integrity before copying
    if (!jpeg_validate_frame(fb)) {
        ESP_LOGW(TAG, "Invalid JPEG, retrying once...");
        esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
        if (!fb || !jpeg_validate_frame(fb)) {
            if (fb) esp_camera_fb_return(fb);
            frame_pool_return(pool_fb); // Release pool buffer
            ESP_LOGE(TAG, "JPEG validation failed twice");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    // 4. Verify size fits in pool buffer
    if (fb->len > pool_fb->capacity) {
        ESP_LOGE(TAG, "Frame too large for pool! (%zu > %zu)", fb->len, pool_fb->capacity);
        esp_camera_fb_return(fb);
        frame_pool_return(pool_fb);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 5. Copy data to pool buffer (Deep Copy)
    //    This decouples the camera driver from the network stack.
    memcpy(pool_fb->buf, fb->buf, fb->len);
    pool_fb->len = fb->len;
    pool_fb->timestamp = fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;

    // 6. Return driver buffer IMMEDIATELY
    //    Camera can now capture the next frame while we send this one.
    esp_camera_fb_return(fb);

    // 7. Send from pool buffer
    //    httpd_resp_send() is synchronous and slow (network speed).
    esp_err_t res = httpd_resp_set_type(req, "image/jpeg");
    if (res == ESP_OK) {
        res = httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    }
    if (res == ESP_OK) {
        res = httpd_resp_send(req, (const char *)pool_fb->buf, pool_fb->len);
    }

    size_t sent_len = pool_fb->len;
    
    // 8. Return pool buffer
    frame_pool_return(pool_fb);

    ESP_LOGI(TAG, "JPEG sent (%zu bytes, heap free: %lu)",
             sent_len, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    return res;
}

// Handler for "/health" endpoint
static esp_err_t health_handler(httpd_req_t *req)
{
    int64_t uptime_s = (esp_timer_get_time() - s_boot_time_us) / 1000000;
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t drops = jpeg_validate_get_drop_count();

    const esp_app_desc_t *app = esp_app_get_description();
    char sha256_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(sha256_hex + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }

    static const char *reset_reasons[] = {
        "unknown", "power_on", "external", "software",
        "panic", "int_watchdog", "task_watchdog", "watchdog",
        "deep_sleep", "brownout", "sdio", "usb", "jtag",
        "efuse", "pwr_glitch", "cpu_lockup"
    };
    esp_reset_reason_t rr = esp_reset_reason();
    const char *reset_str = (rr < (esp_reset_reason_t)(sizeof(reset_reasons)/sizeof(reset_reasons[0])))
                            ? reset_reasons[rr] : "unknown";

    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"uptime_s\":%lld,"
        "\"heap_free\":%zu,"
        "\"internal_free\":%zu,"
        "\"psram_free\":%zu,"
        "\"jpeg_drops\":%lu,"
        "\"reset_reason\":\"%s\","
        "\"app_sha256\":\"%s\"}",
        (long long)uptime_s, free_heap, free_internal, free_psram,
        (unsigned long)drops, reset_str, sha256_hex);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

// Handler for "/stats" endpoint
static esp_err_t stats_handler(httpd_req_t *req)
{
    static uint32_t last_vsync = 0;
    static int64_t last_time = 0;
    static float fps = 0;

    cam_stats_t cam;
    wifi_stats_t wifi;
    esp_camera_get_stats(&cam);
    wifi_get_stats(&wifi);

    int64_t now = esp_timer_get_time();
    if (last_time > 0) {
        uint32_t diff = cam.vsync_isr_count - last_vsync;
        fps = (float)diff / ((now - last_time) / 1000000.0f);
    }
    last_vsync = cam.vsync_isr_count;
    last_time = now;

    char buf[1024];
    int len = snprintf(buf, sizeof(buf),
        "{"
        "\"camera\":{"
            "\"fps\":%.2f,"
            "\"vsync_count\":%lu,"
            "\"eof_count\":%lu,"
            "\"no_soi\":%lu,"
            "\"no_eoi\":%lu,"
            "\"queue_overflow\":%lu,"
            "\"drops_no_buf\":%lu,"
            "\"soi_hist\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu]"
        "},"
        "\"wifi\":{"
            "\"rssi\":%d,"
            "\"disconnects\":%lu,"
            "\"ip\":\"%s\""
        "},"
        "\"memory\":{"
            "\"internal_free\":%zu,"
            "\"psram_free\":%zu"
        "}"
        "}",
        fps, cam.vsync_isr_count, cam.eof_count, cam.no_soi_count, cam.no_eoi_count,
        cam.queue_overflow_count, cam.drops_no_free_buf,
        cam.soi_offset_histogram[0], cam.soi_offset_histogram[1], cam.soi_offset_histogram[2],
        cam.soi_offset_histogram[3], cam.soi_offset_histogram[4], cam.soi_offset_histogram[5],
        cam.soi_offset_histogram[6], cam.soi_offset_histogram[7],
        wifi.rssi, wifi.disconnect_count, wifi.ip,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, len);
}

// ========================================
// MJPEG Stream Handler — /stream
// multipart/x-mixed-replace for Frigate/browsers
// ========================================
static esp_err_t stream_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "MJPEG stream client connected");

    // Set multipart content type
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    char part_hdr[128];
    uint32_t frame_count = 0;
    uint32_t fail_count = 0;
    int64_t start_time = esp_timer_get_time();
    esp_err_t res = ESP_OK;

    while (res == ESP_OK) {
        // OTA needs exclusive flash access — exit stream loop
        if (s_ota_pending) {
            ESP_LOGW(TAG, "OTA pending, stopping stream");
            break;
        }
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            fail_count++;
            if (fail_count >= 3) {
                ESP_LOGW(TAG, "Stream: reinitializing camera after %lu failures",
                         (unsigned long)fail_count);
                camera_reinit();
                fail_count = 0;
                vTaskDelay(pdMS_TO_TICKS(500));
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            continue;
        }

        // Basic JPEG validation
        if (fb->len < 100 ||
            fb->buf[0] != 0xFF || fb->buf[1] != 0xD8 ||
            fb->buf[fb->len - 2] != 0xFF || fb->buf[fb->len - 1] != 0xD9) {
            esp_camera_fb_return(fb);
            fail_count++;
            if (fail_count >= 20) {
                ESP_LOGW(TAG, "Stream: reinitializing camera after %lu bad frames",
                         (unsigned long)fail_count);
                camera_reinit();
                fail_count = 0;
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        fail_count = 0;

        // Copy to pool buffer so camera can continue capturing
        frame_buffer_t *pool_fb = frame_pool_get();
        const uint8_t *send_buf;
        size_t send_len;

        if (pool_fb && fb->len <= pool_fb->capacity) {
            memcpy(pool_fb->buf, fb->buf, fb->len);
            pool_fb->len = fb->len;
            esp_camera_fb_return(fb);
            send_buf = pool_fb->buf;
            send_len = pool_fb->len;
        } else {
            // No pool buffer available — send directly from camera buffer
            if (pool_fb) frame_pool_return(pool_fb);
            pool_fb = NULL;
            send_buf = fb->buf;
            send_len = fb->len;
        }

        // Send multipart boundary + headers
        int hdr_len = snprintf(part_hdr, sizeof(part_hdr), STREAM_PART_HDR, send_len);
        res = httpd_resp_send_chunk(req, part_hdr, hdr_len);

        // Send JPEG data
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)send_buf, send_len);
        }

        // Send trailing CRLF
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, "\r\n", 2);
        }

        // Release buffers
        if (pool_fb) {
            frame_pool_return(pool_fb);
        } else {
            esp_camera_fb_return(fb);
        }

        frame_count++;
        if (frame_count % 100 == 0) {
            int64_t elapsed_s = (esp_timer_get_time() - start_time) / 1000000;
            if (elapsed_s > 0) {
                ESP_LOGI(TAG, "MJPEG stream: %lu frames, %.1f fps, heap: %lu",
                         (unsigned long)frame_count,
                         (float)frame_count / elapsed_s,
                         (unsigned long)esp_get_free_heap_size());
            }
        }

        // Frame pacing
        vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "MJPEG stream client disconnected after %lu frames",
             (unsigned long)frame_count);
    return res;
}

// Handler for "/api/coredump" — stream raw coredump partition
static esp_err_t coredump_handler(httpd_req_t *req)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No coredump partition");
        return ESP_FAIL;
    }

    // Check if there's a valid coredump (first 4 bytes are non-0xFF)
    uint32_t magic = 0;
    esp_partition_read(part, 0, &magic, sizeof(magic));
    if (magic == 0xFFFFFFFF || magic == 0x00000000) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No coredump saved");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Streaming coredump (%lu bytes)", (unsigned long)part->size);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=coredump.bin");

    // Stream in 4KB chunks
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t offset = 0;
    while (offset < part->size) {
        size_t chunk = (part->size - offset > 4096) ? 4096 : (part->size - offset);
        esp_partition_read(part, offset, buf, chunk);
        esp_err_t err = httpd_resp_send_chunk(req, buf, chunk);
        if (err != ESP_OK) {
            free(buf);
            return err;
        }
        offset += chunk;
    }

    free(buf);
    httpd_resp_send_chunk(req, NULL, 0); // End chunked response
    return ESP_OK;
}

// ========================================
// /setup — browser-accessible config page
// ========================================

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], 0};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* GET /setup — HTML config form pre-filled with current values */
static esp_err_t setup_get_handler(httpd_req_t *req)
{
    static const char *res_names[] = {"QVGA (320x240)", "VGA (640x480)", "HD (1280x720)", "UXGA (1600x1200)"};
    static const uint8_t res_vals[] = {6, 10, 13, 15}; // FRAMESIZE_QVGA/VGA/HD/UXGA — PY260-supported only

    /* Get device IP for display */
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) esp_netif_get_ip_info(netif, &ip_info);
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

    char buf[3072];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>Camera Setup</title>"
        "<style>body{font-family:sans-serif;max-width:500px;margin:40px auto;padding:0 16px}"
        "label{display:block;margin-top:12px;font-weight:bold}"
        "input[type=text],select{width:100%%;box-sizing:border-box;padding:6px;margin-top:4px}"
        "input[type=submit]{margin-top:20px;padding:10px 24px;background:#2563eb;color:#fff;"
        "border:none;border-radius:4px;cursor:pointer;font-size:1em}"
        ".info{background:#f0f9ff;border:1px solid #bae6fd;border-radius:4px;"
        "padding:10px 12px;margin-bottom:16px;font-size:0.9em}"
        ".info a{color:#2563eb}"
        "</style></head><body>"
        "<h2>Camera Configuration</h2>"
        "<div class='info'>"
        "Device IP: <strong>%s</strong><br>"
        "Stream: <a href='http://%s:81/stream'>http://%s:81/stream</a>"
        "</div>"
        "<form method='POST' action='/setup'>",
        ip_str, ip_str, ip_str);

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "<label><input type='checkbox' name='mqtt_en' value='1' id='mqtt_en'%s> Enable MQTT</label>"
        "<div id='mqtt_fields'>"
        "<label>MQTT URL</label>"
        "<input type='text' name='mqtt_url' value='%s'>"
        "<label>MQTT Username</label>"
        "<input type='text' name='mqtt_user' value='%s'>"
        "<label>MQTT Password</label>"
        "<input type='password' name='mqtt_pass' value='%s'>"
        "</div>"
        "<label>Device ID "
        "<span title='Used as: mDNS hostname (&lt;id&gt;.local), MQTT topic prefix, "
        "Home Assistant entity prefix, and BLE provisioning name (PROV_&lt;id&gt;). "
        "Use lowercase letters, numbers and underscores only.' "
        "style='font-weight:normal;cursor:help;color:#6b7280'>&#9432;</span>"
        "</label>"
        "<input type='text' name='device_id' value='%s' pattern='[a-z0-9_]+' "
        "title='Lowercase letters, numbers and underscores only'>"
        "<label>Camera Resolution</label>"
        "<select name='cam_res'>",
        config_mgr_is_mqtt_enabled() ? " checked" : "",
        config_mgr_get_mqtt_url(),
        config_mgr_get_mqtt_user(),
        config_mgr_get_mqtt_pass(),
        config_mgr_get_device_id());

    uint8_t cur_res = config_mgr_get_cam_resolution();
    for (int i = 0; i < 4; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "<option value='%u'%s>%s</option>",
            res_vals[i],
            (cur_res == res_vals[i]) ? " selected" : "",
            res_names[i]);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "</select>"
        "<label>JPEG Quality (1=best, 63=lowest)</label>"
        "<input type='text' name='jpeg_qual' value='%u'>"
        "<br><input type='submit' value='Save &amp; Restart'>"
        "<script>function tog(){var e=document.getElementById('mqtt_en').checked;"
        "var d=document.getElementById('mqtt_fields');"
        "d.style.opacity=e?'1':'0.4';"
        "d.querySelectorAll('input').forEach(function(i){i.disabled=!e;});}"
        "document.getElementById('mqtt_en').addEventListener('change',tog);tog();"
        "</script>"
        "</form>"
        "<hr>"
        "<details><summary style='cursor:pointer;color:#dc2626;font-weight:bold'>Danger Zone</summary>"
        "<form method='POST' action='/setup/factory-reset' style='margin-top:12px'>"
        "<p style='color:#dc2626;font-size:0.9em'>Erases all settings and Wi-Fi credentials. "
        "Device will reboot into BLE provisioning mode.</p>"
        "<input type='submit' value='Factory Reset' "
        "onclick=\"return confirm('Erase all NVS data and reboot into BLE provisioning mode?')\" "
        "style='background:#dc2626;color:#fff;border:none;padding:8px 16px;"
        "border-radius:4px;cursor:pointer;font-size:1em'>"
        "</form></details>"
        "</body></html>",
        config_mgr_get_jpeg_quality());

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, buf, pos);
}

/* One-shot task: deinit camera, save config, restart */
static void setup_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_camera_deinit();
    config_mgr_save();
    recovery_mgr_signal_planned_reboot();
    esp_restart();
}

/* One-shot task: deinit camera, erase all NVS, restart into BLE provisioning */
static void factory_reset_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_camera_deinit();
    nvs_flash_erase();
    recovery_mgr_signal_planned_reboot();
    esp_restart();
}

/* POST /setup/factory-reset — erase NVS and reboot into BLE provisioning mode */
static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested — erasing NVS and rebooting");
    char resp_html[512];
    snprintf(resp_html, sizeof(resp_html),
        "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
        "<h2>Factory Reset</h2>"
        "<p>NVS erased. Device is rebooting into BLE provisioning mode.</p>"
        "<p>Use the <strong>Espressif BLE Provisioning</strong> app to reconnect "
        "(<strong>PROV_%s</strong>), then visit /setup to reconfigure.</p>"
        "</body></html>",
        config_mgr_get_device_id());
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp_html, strlen(resp_html));
    xTaskCreate(factory_reset_task, "factory_rst", 4096, NULL, 5, NULL);
    return ESP_OK;
}

/* POST /setup — parse form body, update config, schedule restart */
static esp_err_t setup_post_handler(httpd_req_t *req)
{
#define SETUP_BODY_MAX 512
    char body[SETUP_BODY_MAX + 1];
    int total = req->content_len;
    if (total > SETUP_BODY_MAX) total = SETUP_BODY_MAX;

    int received = httpd_req_recv(req, body, total);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    /* Parse key=value&... pairs */
    char mqtt_url[128]  = {0};
    char mqtt_user[64]  = {0};
    char mqtt_pass[64]  = {0};
    char device_id[32]  = {0};
    bool mqtt_en        = false;
    char cam_res_s[8]   = {0};
    char jpeg_qual_s[8] = {0};

    char *saveptr = NULL;
    char *pair = strtok_r(body, "&", &saveptr);
    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            const char *key = pair;
            const char *val = eq + 1;
            char decoded[128];
            url_decode(decoded, val, sizeof(decoded));

            if (strcmp(key, "mqtt_url")  == 0) strlcpy(mqtt_url,    decoded, sizeof(mqtt_url));
            else if (strcmp(key, "mqtt_user") == 0) strlcpy(mqtt_user,   decoded, sizeof(mqtt_user));
            else if (strcmp(key, "mqtt_pass") == 0) strlcpy(mqtt_pass,   decoded, sizeof(mqtt_pass));
            else if (strcmp(key, "device_id") == 0) strlcpy(device_id,   decoded, sizeof(device_id));
            else if (strcmp(key, "mqtt_en")   == 0) mqtt_en = (decoded[0] == '1');
            else if (strcmp(key, "cam_res")   == 0) strlcpy(cam_res_s,   decoded, sizeof(cam_res_s));
            else if (strcmp(key, "jpeg_qual") == 0) strlcpy(jpeg_qual_s, decoded, sizeof(jpeg_qual_s));
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }

    /* Validate */
    if (device_id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "device_id must not be empty");
        return ESP_FAIL;
    }
    int jpeg_qual = atoi(jpeg_qual_s);
    if (jpeg_qual < 1 || jpeg_qual > 63) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "jpeg_qual must be 1-63");
        return ESP_FAIL;
    }
    int cam_res = atoi(cam_res_s);
    if (cam_res != 6 && cam_res != 10 && cam_res != 13 && cam_res != 15) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid cam_res");
        return ESP_FAIL;
    }

    /* Apply to in-memory config */
    config_mgr_set_mqtt_url(mqtt_url);
    config_mgr_set_mqtt_user(mqtt_user);
    config_mgr_set_mqtt_pass(mqtt_pass);
    config_mgr_set_device_id(device_id);
    config_mgr_set_mqtt_enabled(mqtt_en);
    config_mgr_set_cam_resolution((uint8_t)cam_res);
    config_mgr_set_jpeg_quality((uint8_t)jpeg_qual);

    ESP_LOGI(TAG, "Setup POST: url=%s user=%s dev=%s mqtt_en=%d res=%d qual=%d",
             mqtt_url, mqtt_user, device_id, mqtt_en, cam_res, jpeg_qual);

    /* Send response before restarting */
    static const char *resp_html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
        "<h2>Saved. Rebooting...</h2>"
        "<p>Returning to setup in <span id='c'>30</span>s...</p>"
        "<script>var t=30;function tick(){document.getElementById('c').textContent=t;"
        "if(--t<0){window.location='/setup';return;}setTimeout(tick,1000);}"
        "setTimeout(tick,1000);</script>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp_html, strlen(resp_html));

    /* Deferred restart so the HTTP response is fully sent first */
    xTaskCreate(setup_restart_task, "setup_rst", 4096, NULL, 5, NULL);

    return ESP_OK;
#undef SETUP_BODY_MAX
}

esp_err_t start_stream_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;             // DIFFERENT PORT
    config.ctrl_port = 32767;            // DIFFERENT CONTROL PORT
    config.core_id = 1;                  // Run on Core 1
    config.stack_size = 8192;
    config.max_uri_handlers = 5;
    config.max_open_sockets = 5;         
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting Stream Server on port: '%d'", config.server_port);

    if (httpd_start(&s_stream_server, &config) == ESP_OK) {
        httpd_uri_t stream_uri = {
            .uri       = "/stream",
            .method    = HTTP_GET,
            .handler   = stream_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_stream_server, &stream_uri);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Error starting stream server!");
    return ESP_FAIL;
}

httpd_handle_t stream_server_get_handle(void)
{
    return s_stream_server;
}

esp_err_t start_http_server(void)
{
    s_boot_time_us = esp_timer_get_time();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 1;
    config.stack_size = 8192;
    config.max_uri_handlers = 10;
    config.max_open_sockets = 10;
    config.recv_wait_timeout = 5;   // 5s recv timeout to prevent WDT panics on network stall
    config.send_wait_timeout = 5;   // 5s send timeout to prevent WDT panics on network stall
    config.lru_purge_enable = true;  // Purge least-recently-used connections

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d' (Core: %d)", config.server_port, config.core_id);

    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_uri_t capture_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = capture_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &capture_uri);

        httpd_uri_t health_uri = {
            .uri       = "/health",
            .method    = HTTP_GET,
            .handler   = health_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &health_uri);

        httpd_uri_t stats_uri = {
            .uri       = "/stats",
            .method    = HTTP_GET,
            .handler   = stats_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &stats_uri);

        httpd_uri_t coredump_uri = {
            .uri       = "/api/coredump",
            .method    = HTTP_GET,
            .handler   = coredump_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &coredump_uri);

        httpd_uri_t setup_get_uri = {
            .uri       = "/setup",
            .method    = HTTP_GET,
            .handler   = setup_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &setup_get_uri);

        httpd_uri_t setup_post_uri = {
            .uri       = "/setup",
            .method    = HTTP_POST,
            .handler   = setup_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &setup_post_uri);

        httpd_uri_t factory_reset_uri = {
            .uri       = "/setup/factory-reset",
            .method    = HTTP_POST,
            .handler   = factory_reset_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_server, &factory_reset_uri);

        return ESP_OK;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return ESP_FAIL;
}

httpd_handle_t http_server_get_handle(void)
{
    return s_server;
}

void http_server_stop(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Stopping main HTTP server (Port 80)...");
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_stream_server) {
        ESP_LOGW(TAG, "Stopping stream HTTP server (Port 81)...");
        httpd_stop(s_stream_server);
        s_stream_server = NULL;
    }
}

void http_server_signal_stop(void)
{
    ESP_LOGW(TAG, "Signaling HTTP streams to stop...");
    s_ota_pending = true;
}

void http_server_prepare_ota(void)
{
    ESP_LOGW(TAG, "OTA requested: signaling streams to stop...");
    s_ota_pending = true;

    // Wait for stream handler to notice the flag and exit.
    // Stream handler checks every STREAM_FRAME_INTERVAL_MS (100ms)
    // plus camera_fb_get can block up to ~200ms.
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Drain all pending camera frames before deinit.
    // cam_task on Core 1 may be mid-frame (adjusting SOI offsets on buffer
    // pointers). By draining, we ensure cam_task finishes processing and
    // blocks on its empty queue — safe to delete from another core.
    ESP_LOGW(TAG, "Draining camera frame buffers...");
    for (int i = 0; i < 10; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        esp_camera_fb_return(fb);
    }
    // One more delay to let cam_task settle back to queue-wait state
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGW(TAG, "Deinitializing camera for OTA...");
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Camera stopped. Flash is safe for OTA writes.");
}
