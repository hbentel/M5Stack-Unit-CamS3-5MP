#include "config_mgr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config_mgr";

#define NVS_NAMESPACE "app_cfg"

/* NVS key names */
#define KEY_MQTT_URL   "mqtt_url"
#define KEY_MQTT_USER  "mqtt_user"
#define KEY_MQTT_PASS  "mqtt_pass"
#define KEY_DEVICE_ID  "device_id"
#define KEY_MQTT_EN    "mqtt_en"
#define KEY_CAM_RES    "cam_res"
#define KEY_JPEG_QUAL  "jpeg_qual"

/* Field size limits (including null terminator) */
#define MQTT_URL_MAX   128
#define MQTT_USER_MAX  64
#define MQTT_PASS_MAX  64
#define DEVICE_ID_MAX  32

/* Default framesize: FRAMESIZE_VGA = 8 */
#define DEFAULT_CAM_RES   8
#define DEFAULT_JPEG_QUAL 12

/* In-memory config state */
static char s_mqtt_url[MQTT_URL_MAX];
static char s_mqtt_user[MQTT_USER_MAX];
static char s_mqtt_pass[MQTT_PASS_MAX];
static char s_device_id[DEVICE_ID_MAX];
static bool    s_mqtt_en;
static uint8_t s_cam_res;
static uint8_t s_jpeg_qual;

static bool s_initialized = false;

/* Load a string key; on ESP_ERR_NVS_NOT_FOUND use the fallback */
static void load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_size, const char *fallback)
{
    size_t len = dst_size;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strlcpy(dst, fallback ? fallback : "", dst_size);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_str(%s) err=0x%x, using fallback", key, err);
        strlcpy(dst, fallback ? fallback : "", dst_size);
    }
}

/* Load a u8 key; on ESP_ERR_NVS_NOT_FOUND use the fallback */
static void load_u8(nvs_handle_t h, const char *key, uint8_t *dst, uint8_t fallback)
{
    esp_err_t err = nvs_get_u8(h, key, dst);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *dst = fallback;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_u8(%s) err=0x%x, using fallback", key, err);
        *dst = fallback;
    }
}

esp_err_t config_mgr_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_open(%s) err=0x%x — using all Kconfig defaults", NVS_NAMESPACE, err);
    }

    bool nvs_ok = (err == ESP_OK);
    nvs_handle_t dummy = 0;
    nvs_handle_t nh = nvs_ok ? h : dummy;

    /* String fields with Kconfig fallbacks */
    if (nvs_ok) {
        load_str(nh, KEY_MQTT_URL,  s_mqtt_url,  sizeof(s_mqtt_url),  CONFIG_UNITCAMS3_MQTT_BROKER_URL);
        load_str(nh, KEY_MQTT_USER, s_mqtt_user, sizeof(s_mqtt_user), CONFIG_UNITCAMS3_MQTT_USER);
        load_str(nh, KEY_MQTT_PASS, s_mqtt_pass, sizeof(s_mqtt_pass), CONFIG_UNITCAMS3_MQTT_PASS);
        load_str(nh, KEY_DEVICE_ID, s_device_id, sizeof(s_device_id), CONFIG_UNITCAMS3_DEVICE_ID);
    } else {
        strlcpy(s_mqtt_url,  CONFIG_UNITCAMS3_MQTT_BROKER_URL, sizeof(s_mqtt_url));
        strlcpy(s_mqtt_user, CONFIG_UNITCAMS3_MQTT_USER,       sizeof(s_mqtt_user));
        strlcpy(s_mqtt_pass, CONFIG_UNITCAMS3_MQTT_PASS,       sizeof(s_mqtt_pass));
        strlcpy(s_device_id, CONFIG_UNITCAMS3_DEVICE_ID,       sizeof(s_device_id));
        s_mqtt_en  = true;
        s_cam_res  = DEFAULT_CAM_RES;
        s_jpeg_qual = DEFAULT_JPEG_QUAL;
        s_initialized = true;
        return ESP_OK;
    }

    /* Numeric fields */
    uint8_t mqtt_en_u8 = 1;
    load_u8(nh, KEY_MQTT_EN,   &mqtt_en_u8,   1);
    load_u8(nh, KEY_CAM_RES,   &s_cam_res,    DEFAULT_CAM_RES);
    load_u8(nh, KEY_JPEG_QUAL, &s_jpeg_qual,  DEFAULT_JPEG_QUAL);
    s_mqtt_en = (mqtt_en_u8 != 0);

    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded: url=%s user=%s dev=%s mqtt_en=%d res=%u qual=%u",
             s_mqtt_url, s_mqtt_user, s_device_id, s_mqtt_en, s_cam_res, s_jpeg_qual);

    s_initialized = true;
    return ESP_OK;
}

/* --- Getters --- */

const char *config_mgr_get_mqtt_url(void)      { return s_mqtt_url; }
const char *config_mgr_get_mqtt_user(void)     { return s_mqtt_user; }
const char *config_mgr_get_mqtt_pass(void)     { return s_mqtt_pass; }
const char *config_mgr_get_device_id(void)     { return s_device_id; }
bool        config_mgr_is_mqtt_enabled(void)   { return s_mqtt_en; }
uint8_t     config_mgr_get_cam_resolution(void){ return s_cam_res; }
uint8_t     config_mgr_get_jpeg_quality(void)  { return s_jpeg_qual; }

/* --- Setters (update in-memory only) --- */

void config_mgr_set_mqtt_url(const char *v)      { strlcpy(s_mqtt_url,  v, sizeof(s_mqtt_url)); }
void config_mgr_set_mqtt_user(const char *v)     { strlcpy(s_mqtt_user, v, sizeof(s_mqtt_user)); }
void config_mgr_set_mqtt_pass(const char *v)     { strlcpy(s_mqtt_pass, v, sizeof(s_mqtt_pass)); }
void config_mgr_set_device_id(const char *v)     { strlcpy(s_device_id, v, sizeof(s_device_id)); }
void config_mgr_set_mqtt_enabled(bool v)         { s_mqtt_en = v; }
void config_mgr_set_cam_resolution(uint8_t v)    { s_cam_res = v; }
void config_mgr_set_jpeg_quality(uint8_t v)      { s_jpeg_qual = v; }

/* --- Persist to NVS --- */

esp_err_t config_mgr_save(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "config_mgr_save called before init");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(READWRITE) err=0x%x", err);
        return err;
    }

    nvs_set_str(h, KEY_MQTT_URL,  s_mqtt_url);
    nvs_set_str(h, KEY_MQTT_USER, s_mqtt_user);
    nvs_set_str(h, KEY_MQTT_PASS, s_mqtt_pass);
    nvs_set_str(h, KEY_DEVICE_ID, s_device_id);
    nvs_set_u8(h,  KEY_MQTT_EN,   s_mqtt_en ? 1 : 0);
    nvs_set_u8(h,  KEY_CAM_RES,   s_cam_res);
    nvs_set_u8(h,  KEY_JPEG_QUAL, s_jpeg_qual);

    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit err=0x%x", err);
    } else {
        ESP_LOGI(TAG, "Config saved to NVS");
    }
    return err;
}
