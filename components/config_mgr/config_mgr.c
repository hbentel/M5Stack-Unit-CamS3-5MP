#include "config_mgr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_rom_crc.h"
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
#define KEY_CAM_QUAL   "jpeg_qual"   /* key name kept for NVS compatibility */
#define KEY_OTA_TOKEN  "ota_token"
#define KEY_CD_TOKEN   "cd_token"

/* Field size limits (including null terminator) */
#define MQTT_URL_MAX   128
#define MQTT_USER_MAX  64
#define MQTT_PASS_MAX  64
#define DEVICE_ID_MAX  32
#define OTA_TOKEN_MAX  64
#define CD_TOKEN_MAX   64

/* Default framesize: FRAMESIZE_VGA = 10 */
#define DEFAULT_CAM_RES   10
/* PY260 JPEG compression level: 0 = High, 1 = Default, 2 = Low (mega_ccm.c set_quality) */
#define DEFAULT_CAM_QUAL  1
#define CAM_QUAL_MAX      2

/* In-memory config state */
static char s_mqtt_url[MQTT_URL_MAX];
static char s_mqtt_user[MQTT_USER_MAX];
static char s_mqtt_pass[MQTT_PASS_MAX];
static char s_device_id[DEVICE_ID_MAX];
static char s_ota_token[OTA_TOKEN_MAX];
static char s_cd_token[CD_TOKEN_MAX];
static bool    s_mqtt_en;
static uint8_t s_cam_res;
static uint8_t s_cam_qual;

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

/* PY260-supported framesizes only; excludes SVGA(11) and XGA(12) which
 * mega_ccm.c does not handle. Shared by config_mgr_init() validation and the
 * cam_resolution setter so bad values cannot enter from any call site. */
static bool cam_res_is_valid(uint8_t v)
{
    static const uint8_t valid_res[] = {0, 1, 6, 7, 10, 13, 15, 16, 24};
    for (size_t i = 0; i < sizeof(valid_res); i++) {
        if (v == valid_res[i]) return true;
    }
    return false;
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
        load_str(nh, KEY_OTA_TOKEN, s_ota_token, sizeof(s_ota_token), CONFIG_UNITCAMS3_OTA_TOKEN);
        load_str(nh, KEY_CD_TOKEN,  s_cd_token,  sizeof(s_cd_token),  CONFIG_UNITCAMS3_COREDUMP_TOKEN);
    } else {
        strlcpy(s_mqtt_url,  CONFIG_UNITCAMS3_MQTT_BROKER_URL, sizeof(s_mqtt_url));
        strlcpy(s_mqtt_user, CONFIG_UNITCAMS3_MQTT_USER,       sizeof(s_mqtt_user));
        strlcpy(s_mqtt_pass, CONFIG_UNITCAMS3_MQTT_PASS,       sizeof(s_mqtt_pass));
        strlcpy(s_device_id, CONFIG_UNITCAMS3_DEVICE_ID,       sizeof(s_device_id));
        strlcpy(s_ota_token, CONFIG_UNITCAMS3_OTA_TOKEN,       sizeof(s_ota_token));
        strlcpy(s_cd_token,  CONFIG_UNITCAMS3_COREDUMP_TOKEN,  sizeof(s_cd_token));
        s_mqtt_en  = true;
        s_cam_res  = DEFAULT_CAM_RES;
        s_cam_qual = DEFAULT_CAM_QUAL;
        s_initialized = true;
        return ESP_OK;
    }

    /* Numeric fields */
    uint8_t mqtt_en_u8 = 1;
    load_u8(nh, KEY_MQTT_EN,   &mqtt_en_u8,   1);
    load_u8(nh, KEY_CAM_RES,   &s_cam_res,    DEFAULT_CAM_RES);
    load_u8(nh, KEY_CAM_QUAL,  &s_cam_qual,   DEFAULT_CAM_QUAL);
    s_mqtt_en = (mqtt_en_u8 != 0);

    /* Validate cam_res against PY260-supported framesizes.
     * Reject values that mega_ccm.c does not handle — previously stored
     * values (e.g. 8=CIF, 9=HVGA from before the enum fix) would cause
     * esp_camera_init() to fail. Reset to default if invalid. */
    if (!cam_res_is_valid(s_cam_res)) {
        ESP_LOGW(TAG, "cam_res=%u not supported by PY260 — resetting to VGA (%u)",
                 s_cam_res, DEFAULT_CAM_RES);
        s_cam_res = DEFAULT_CAM_RES;
    }

    /* Older firmware stored a 1-63 "JPEG quality" here that was never
     * applied to the sensor (which only has 3 levels). Any out-of-range value is
     * such a legacy setting — map it to Default, which is what was in effect. */
    if (s_cam_qual > CAM_QUAL_MAX) {
        ESP_LOGW(TAG, "legacy jpeg_qual=%u — using Default quality (%u)", s_cam_qual, DEFAULT_CAM_QUAL);
        s_cam_qual = DEFAULT_CAM_QUAL;
    }

    nvs_close(h);

    ESP_LOGI(TAG, "Config loaded: url=%s user=%s dev=%s mqtt_en=%d res=%u qual=%u",
             s_mqtt_url, s_mqtt_user, s_device_id, s_mqtt_en, s_cam_res, s_cam_qual);

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
uint8_t     config_mgr_get_cam_quality(void)   { return s_cam_qual; }
const char *config_mgr_get_ota_token(void)     { return s_ota_token; }
const char *config_mgr_get_coredump_token(void) { return s_cd_token; }

/* --- Setters (update in-memory only) --- */

void config_mgr_set_mqtt_url(const char *v)      { strlcpy(s_mqtt_url,  v, sizeof(s_mqtt_url)); }
void config_mgr_set_mqtt_user(const char *v)     { strlcpy(s_mqtt_user, v, sizeof(s_mqtt_user)); }
void config_mgr_set_mqtt_pass(const char *v)     { strlcpy(s_mqtt_pass, v, sizeof(s_mqtt_pass)); }
void config_mgr_set_device_id(const char *v)     { strlcpy(s_device_id, v, sizeof(s_device_id)); }
void config_mgr_set_mqtt_enabled(bool v)         { s_mqtt_en = v; }
void config_mgr_set_cam_resolution(uint8_t v)
{
    if (!cam_res_is_valid(v)) {
        ESP_LOGW(TAG, "set_cam_resolution: %u not PY260-supported — ignoring", v);
        return;
    }
    s_cam_res = v;
}
void config_mgr_set_cam_quality(uint8_t v)
{
    if (v > CAM_QUAL_MAX) {
        ESP_LOGW(TAG, "set_cam_quality: %u out of range (0-%u) — ignoring", v, CAM_QUAL_MAX);
        return;
    }
    s_cam_qual = v;
}
void config_mgr_set_ota_token(const char *v)     { strlcpy(s_ota_token, v, sizeof(s_ota_token)); }
void config_mgr_set_coredump_token(const char *v) { strlcpy(s_cd_token,  v, sizeof(s_cd_token)); }

/* --- Persist to NVS --- */

/* Writes the in-memory config to NVS. Only call this before Wi-Fi starts (see
 * config_mgr_apply_staged()) — a flash write disables the OPI PSRAM cache, and
 * doing it with Wi-Fi/MQTT running has crashed this device (v0.2.5, v0.2.7). */
static esp_err_t write_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(READWRITE) err=0x%x", err);
        return err;
    }

    /* Abort on the first failed write so a half-updated config is never
     * committed. nvs_set_* only stages values; commit makes them durable. */
    err = nvs_set_str(h, KEY_MQTT_URL,  s_mqtt_url);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_USER, s_mqtt_user);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_MQTT_PASS, s_mqtt_pass);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_DEVICE_ID, s_device_id);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_OTA_TOKEN, s_ota_token);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_CD_TOKEN,  s_cd_token);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_MQTT_EN,   s_mqtt_en ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_CAM_RES,   s_cam_res);
    if (err == ESP_OK) err = nvs_set_u8(h,  KEY_CAM_QUAL,  s_cam_qual);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set failed err=0x%x — aborting save without commit", err);
        nvs_close(h);
        return err;
    }

    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit err=0x%x", err);
    } else {
        ESP_LOGI(TAG, "Config saved to NVS");
    }
    return err;
}

/* --- Deferred save / factory reset via RTC RAM ---
 *
 * /setup runs while Wi-Fi is up, where flash writes are unsafe. Instead of
 * writing NVS there, the new config (or a factory-reset request) is staged in
 * RTC_NOINIT memory — a plain SRAM write that survives esp_restart() — and
 * committed on the next boot, before Wi-Fi starts. Same pattern as the OTA URL
 * (ota_mgr.c) and Wi-Fi reprovisioning (wifi.c).
 */
#define STAGED_CONFIG_MAGIC         0xC0F16A01UL
#define STAGED_FACTORY_RESET_MAGIC  0xFAC70A55UL

typedef struct {
    char    mqtt_url[MQTT_URL_MAX];
    char    mqtt_user[MQTT_USER_MAX];
    char    mqtt_pass[MQTT_PASS_MAX];
    char    device_id[DEVICE_ID_MAX];
    char    ota_token[OTA_TOKEN_MAX];
    char    cd_token[CD_TOKEN_MAX];
    uint8_t mqtt_en;
    uint8_t cam_res;
    uint8_t cam_qual;
} staged_cfg_t;

static RTC_NOINIT_ATTR uint32_t     s_staged_magic;
static RTC_NOINIT_ATTR uint32_t     s_staged_check;  /* CRC32 of cfg, or ~magic for factory reset */
static RTC_NOINIT_ATTR staged_cfg_t s_staged_cfg;

static uint32_t staged_crc(void)
{
    return esp_rom_crc32_le(0, (const uint8_t *)&s_staged_cfg, sizeof(s_staged_cfg));
}

esp_err_t config_mgr_stage_for_reboot(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "config_mgr_stage_for_reboot called before init");
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_staged_cfg.mqtt_url,  s_mqtt_url,  sizeof(s_staged_cfg.mqtt_url));
    strlcpy(s_staged_cfg.mqtt_user, s_mqtt_user, sizeof(s_staged_cfg.mqtt_user));
    strlcpy(s_staged_cfg.mqtt_pass, s_mqtt_pass, sizeof(s_staged_cfg.mqtt_pass));
    strlcpy(s_staged_cfg.device_id, s_device_id, sizeof(s_staged_cfg.device_id));
    strlcpy(s_staged_cfg.ota_token, s_ota_token, sizeof(s_staged_cfg.ota_token));
    strlcpy(s_staged_cfg.cd_token,  s_cd_token,  sizeof(s_staged_cfg.cd_token));
    s_staged_cfg.mqtt_en  = s_mqtt_en ? 1 : 0;
    s_staged_cfg.cam_res  = s_cam_res;
    s_staged_cfg.cam_qual = s_cam_qual;
    s_staged_check = staged_crc();
    s_staged_magic = STAGED_CONFIG_MAGIC;
    ESP_LOGI(TAG, "Config staged in RTC RAM — will be saved to NVS on next boot");
    return ESP_OK;
}

void config_mgr_stage_factory_reset(void)
{
    s_staged_check = ~STAGED_FACTORY_RESET_MAGIC;
    s_staged_magic = STAGED_FACTORY_RESET_MAGIC;
    ESP_LOGW(TAG, "Factory reset staged in RTC RAM — NVS will be erased on next boot");
}

esp_err_t config_mgr_apply_staged(void)
{
    uint32_t magic = s_staged_magic;
    uint32_t check = s_staged_check;
    s_staged_magic = 0;  /* clear first: a crash below must not repeat the action every boot */

    if (magic == STAGED_FACTORY_RESET_MAGIC && check == ~STAGED_FACTORY_RESET_MAGIC) {
        ESP_LOGW(TAG, "Applying staged factory reset — erasing NVS");
        nvs_flash_deinit();
        esp_err_t err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
        if (err != ESP_OK) ESP_LOGE(TAG, "Factory reset failed: 0x%x", err);
        return err;
    }

    if (magic != STAGED_CONFIG_MAGIC || check != staged_crc()) {
        return ESP_OK;  /* nothing staged (or stale/corrupt RTC contents after power-on) */
    }

    strlcpy(s_mqtt_url,  s_staged_cfg.mqtt_url,  sizeof(s_mqtt_url));
    strlcpy(s_mqtt_user, s_staged_cfg.mqtt_user, sizeof(s_mqtt_user));
    strlcpy(s_mqtt_pass, s_staged_cfg.mqtt_pass, sizeof(s_mqtt_pass));
    strlcpy(s_device_id, s_staged_cfg.device_id, sizeof(s_device_id));
    strlcpy(s_ota_token, s_staged_cfg.ota_token, sizeof(s_ota_token));
    strlcpy(s_cd_token,  s_staged_cfg.cd_token,  sizeof(s_cd_token));
    s_mqtt_en  = s_staged_cfg.mqtt_en != 0;
    s_cam_res  = cam_res_is_valid(s_staged_cfg.cam_res) ? s_staged_cfg.cam_res : DEFAULT_CAM_RES;
    s_cam_qual = (s_staged_cfg.cam_qual <= CAM_QUAL_MAX) ? s_staged_cfg.cam_qual : DEFAULT_CAM_QUAL;

    ESP_LOGI(TAG, "Applying staged config from previous boot");
    return write_nvs();
}
