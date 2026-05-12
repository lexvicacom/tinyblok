#include "tinyblok_config.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#ifndef CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
#define CONFIG_TINYBLOK_NATS_CREDS_SUPPORT 0
#endif
#ifndef CONFIG_TINYBLOK_NATS_TLS
#define CONFIG_TINYBLOK_NATS_TLS 0
#endif
#ifndef CONFIG_TINYBLOK_DISPLAY_ENABLED_DEFAULT
#define CONFIG_TINYBLOK_DISPLAY_ENABLED_DEFAULT 0
#endif

static const char *TAG = "tinyblok_config";
static const char *NVS_NS = "tinyblok";

#define SAMPLE_INTERVAL_MIN_MS 100U

static esp_err_t set_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || !src)
        return ESP_ERR_INVALID_ARG;
    size_t len = strnlen(src, dst_len);
    if (len >= dst_len)
        return ESP_ERR_INVALID_SIZE;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return ESP_OK;
}

static esp_err_t validate_config(const tinyblok_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "config is null");
    ESP_RETURN_ON_FALSE(strnlen(cfg->device_name, sizeof(cfg->device_name)) < sizeof(cfg->device_name),
                        ESP_ERR_INVALID_SIZE, TAG, "device_name too long");
    ESP_RETURN_ON_FALSE(strnlen(cfg->wifi_ssid, sizeof(cfg->wifi_ssid)) < sizeof(cfg->wifi_ssid),
                        ESP_ERR_INVALID_SIZE, TAG, "wifi_ssid too long");
    ESP_RETURN_ON_FALSE(strnlen(cfg->wifi_password, sizeof(cfg->wifi_password)) < sizeof(cfg->wifi_password),
                        ESP_ERR_INVALID_SIZE, TAG, "wifi_password too long");
    ESP_RETURN_ON_FALSE(strnlen(cfg->nats_host, sizeof(cfg->nats_host)) < sizeof(cfg->nats_host),
                        ESP_ERR_INVALID_SIZE, TAG, "nats_host too long");
    ESP_RETURN_ON_FALSE(strnlen(cfg->nats_user, sizeof(cfg->nats_user)) < sizeof(cfg->nats_user),
                        ESP_ERR_INVALID_SIZE, TAG, "nats_user too long");
    ESP_RETURN_ON_FALSE(strnlen(cfg->nats_password, sizeof(cfg->nats_password)) < sizeof(cfg->nats_password),
                        ESP_ERR_INVALID_SIZE, TAG, "nats_password too long");
    ESP_RETURN_ON_FALSE(strnlen(cfg->nats_token, sizeof(cfg->nats_token)) < sizeof(cfg->nats_token),
                        ESP_ERR_INVALID_SIZE, TAG, "nats_token too long");
    ESP_RETURN_ON_FALSE(strnlen(cfg->nats_base_topic, sizeof(cfg->nats_base_topic)) < sizeof(cfg->nats_base_topic),
                        ESP_ERR_INVALID_SIZE, TAG, "nats_base_topic too long");
    ESP_RETURN_ON_FALSE(strnlen(cfg->nats_auth, sizeof(cfg->nats_auth)) < sizeof(cfg->nats_auth),
                        ESP_ERR_INVALID_SIZE, TAG, "nats_auth too long");
    ESP_RETURN_ON_FALSE(strcmp(cfg->nats_auth, "none") == 0 || strcmp(cfg->nats_auth, "userpass") == 0 ||
                            strcmp(cfg->nats_auth, "token") == 0 || strcmp(cfg->nats_auth, "creds") == 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid nats_auth");
#if !CONFIG_TINYBLOK_NATS_TLS
    ESP_RETURN_ON_FALSE(strcmp(cfg->nats_auth, "creds") != 0 && !cfg->nats_tls,
                        ESP_ERR_NOT_SUPPORTED, TAG, "TLS support is not compiled in");
#endif
#if !CONFIG_TINYBLOK_NATS_CREDS_SUPPORT
    ESP_RETURN_ON_FALSE(strcmp(cfg->nats_auth, "creds") != 0,
                        ESP_ERR_NOT_SUPPORTED, TAG, ".creds support is not compiled in");
#endif
    ESP_RETURN_ON_FALSE(strcmp(cfg->nats_auth, "creds") != 0 || cfg->nats_tls,
                        ESP_ERR_INVALID_ARG, TAG, ".creds auth requires TLS");
    ESP_RETURN_ON_FALSE(strnlen(cfg->nats_creds, sizeof(cfg->nats_creds)) < sizeof(cfg->nats_creds),
                        ESP_ERR_INVALID_SIZE, TAG, "nats_creds too long");
    ESP_RETURN_ON_FALSE(strcmp(cfg->nats_auth, "userpass") != 0 || cfg->nats_user[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "nats_user is required for userpass auth");
    ESP_RETURN_ON_FALSE(strcmp(cfg->nats_auth, "token") != 0 || cfg->nats_token[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "nats_token is required for token auth");
    ESP_RETURN_ON_FALSE(strcmp(cfg->nats_auth, "creds") != 0 || cfg->nats_creds[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "nats_creds is required for .creds auth");
    ESP_RETURN_ON_FALSE(strnlen(cfg->timezone, sizeof(cfg->timezone)) < sizeof(cfg->timezone),
                        ESP_ERR_INVALID_SIZE, TAG, "timezone too long");
    ESP_RETURN_ON_FALSE(cfg->nats_port > 0, ESP_ERR_INVALID_ARG, TAG, "nats_port out of range");
    ESP_RETURN_ON_FALSE(cfg->sample_interval_ms >= SAMPLE_INTERVAL_MIN_MS,
                        ESP_ERR_INVALID_ARG, TAG, "sample interval too small");
    return ESP_OK;
}

void tinyblok_config_load_defaults(tinyblok_config_t *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    (void)set_string(cfg->device_name, sizeof(cfg->device_name), CONFIG_TINYBLOK_DEVICE_NAME);
    (void)set_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), CONFIG_TINYBLOK_WIFI_SSID);
    (void)set_string(cfg->wifi_password, sizeof(cfg->wifi_password), CONFIG_TINYBLOK_WIFI_PASSWORD);
    (void)set_string(cfg->nats_host, sizeof(cfg->nats_host), CONFIG_TINYBLOK_NATS_HOST);
    cfg->nats_port = (uint16_t)CONFIG_TINYBLOK_NATS_PORT;
    (void)set_string(cfg->nats_base_topic, sizeof(cfg->nats_base_topic), CONFIG_TINYBLOK_NATS_BASE_TOPIC);
    (void)set_string(cfg->nats_auth, sizeof(cfg->nats_auth), "none");
    cfg->nats_tls = CONFIG_TINYBLOK_NATS_TLS != 0;
    cfg->display_enabled = CONFIG_TINYBLOK_DISPLAY_ENABLED_DEFAULT != 0;
    cfg->display_brightness = (uint8_t)CONFIG_TINYBLOK_DISPLAY_BRIGHTNESS_DEFAULT;
    cfg->sample_interval_ms = CONFIG_TINYBLOK_SAMPLE_INTERVAL_MS;
    (void)set_string(cfg->timezone, sizeof(cfg->timezone), CONFIG_TINYBLOK_TIMEZONE);
    cfg->configured = false;
}

esp_err_t tinyblok_config_init(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return err;
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t get_string(nvs_handle_t nvs, const char *key, char *dst, size_t dst_len)
{
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(nvs, key, dst, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (err == ESP_OK && len > dst_len)
        return ESP_ERR_INVALID_SIZE;
    return err;
}

static esp_err_t get_string_fallback(nvs_handle_t nvs, const char *key, const char *old_key,
                                     char *dst, size_t dst_len)
{
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(nvs, key, dst, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND && old_key)
    {
        len = dst_len;
        err = nvs_get_str(nvs, old_key, dst, &len);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (err == ESP_OK && len > dst_len)
        return ESP_ERR_INVALID_SIZE;
    return err;
}

esp_err_t tinyblok_config_load(tinyblok_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "config is null");
    tinyblok_config_load_defaults(cfg);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    ESP_RETURN_ON_ERROR(err, TAG, "open config namespace");

    err = get_string(nvs, "device_name", cfg->device_name, sizeof(cfg->device_name));
    if (err == ESP_OK)
        err = get_string(nvs, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    if (err == ESP_OK)
        err = get_string(nvs, "wifi_password", cfg->wifi_password, sizeof(cfg->wifi_password));
    if (err == ESP_OK)
        err = get_string_fallback(nvs, "nats_host", "mqtt_host", cfg->nats_host, sizeof(cfg->nats_host));
    if (err == ESP_OK)
        err = get_string_fallback(nvs, "nats_user", "mqtt_user", cfg->nats_user, sizeof(cfg->nats_user));
    if (err == ESP_OK)
        err = get_string_fallback(nvs, "nats_password", "mqtt_password", cfg->nats_password, sizeof(cfg->nats_password));
    if (err == ESP_OK)
        err = get_string_fallback(nvs, "nats_token", "mqtt_token", cfg->nats_token, sizeof(cfg->nats_token));
    if (err == ESP_OK)
        err = get_string_fallback(nvs, "nats_base", "mqtt_base", cfg->nats_base_topic, sizeof(cfg->nats_base_topic));
    if (err == ESP_OK)
        err = get_string(nvs, "nats_auth", cfg->nats_auth, sizeof(cfg->nats_auth));
    if (err == ESP_OK)
        err = get_string(nvs, "nats_creds", cfg->nats_creds, sizeof(cfg->nats_creds));
    if (err == ESP_OK)
        err = get_string(nvs, "timezone", cfg->timezone, sizeof(cfg->timezone));

    uint16_t nats_port = cfg->nats_port;
    uint8_t display_enabled = cfg->display_enabled ? 1 : 0;
    uint8_t display_brightness = cfg->display_brightness;
    uint8_t nats_tls = cfg->nats_tls ? 1 : 0;
    uint32_t sample_interval_ms = cfg->sample_interval_ms;
    uint8_t configured = cfg->configured ? 1 : 0;
    if (err == ESP_OK)
        err = nvs_get_u16(nvs, "nats_port", &nats_port);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = nvs_get_u16(nvs, "mqtt_port", &nats_port);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_get_u8(nvs, "disp_enabled", &display_enabled);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_get_u8(nvs, "disp_bright", &display_brightness);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_get_u8(nvs, "nats_tls", &nats_tls);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_get_u32(nvs, "sample_ms", &sample_interval_ms);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_get_u8(nvs, "configured", &configured);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;

    nvs_close(nvs);
    if (err != ESP_OK)
        return err;

    cfg->nats_port = nats_port;
    cfg->display_enabled = display_enabled != 0;
    cfg->display_brightness = display_brightness;
    cfg->nats_tls = nats_tls != 0;
    cfg->sample_interval_ms = sample_interval_ms;
    cfg->configured = configured != 0;

    if (cfg->wifi_ssid[0] == '\0')
        cfg->configured = false;

    ESP_RETURN_ON_ERROR(validate_config(cfg), TAG, "validate loaded config");
    ESP_LOGI(TAG, "loaded config: configured=%d device='%s' wifi_ssid='%s' nats_host='%s' nats_port=%u",
             cfg->configured, cfg->device_name, cfg->wifi_ssid, cfg->nats_host, cfg->nats_port);
    return ESP_OK;
}

esp_err_t tinyblok_config_save(const tinyblok_config_t *cfg)
{
    ESP_RETURN_ON_ERROR(validate_config(cfg), TAG, "validate config");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "open config namespace");
    esp_err_t err = nvs_set_str(nvs, "device_name", cfg->device_name);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "wifi_ssid", cfg->wifi_ssid);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "wifi_password", cfg->wifi_password);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "nats_host", cfg->nats_host);
    if (err == ESP_OK)
        err = nvs_set_u16(nvs, "nats_port", cfg->nats_port);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "nats_user", cfg->nats_user);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "nats_password", cfg->nats_password);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "nats_token", cfg->nats_token);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "nats_base", cfg->nats_base_topic);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "nats_auth", cfg->nats_auth);
    if (err == ESP_OK)
        err = nvs_set_u8(nvs, "nats_tls", cfg->nats_tls ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "nats_creds", cfg->nats_creds);
    if (err == ESP_OK)
        err = nvs_set_u8(nvs, "disp_enabled", cfg->display_enabled ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_set_u8(nvs, "disp_bright", cfg->display_brightness);
    if (err == ESP_OK)
        err = nvs_set_u32(nvs, "sample_ms", cfg->sample_interval_ms);
    if (err == ESP_OK)
        err = nvs_set_str(nvs, "timezone", cfg->timezone);
    if (err == ESP_OK)
        err = nvs_set_u8(nvs, "configured", cfg->configured ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "save config");
    ESP_LOGI(TAG, "saved config: configured=%d device='%s' wifi_ssid='%s' nats_host='%s' nats_port=%u",
             cfg->configured, cfg->device_name, cfg->wifi_ssid, cfg->nats_host, cfg->nats_port);
    return ESP_OK;
}

esp_err_t tinyblok_config_reset(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    ESP_RETURN_ON_ERROR(err, TAG, "open config namespace");
    err = nvs_erase_all(nvs);
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "reset config");
    ESP_LOGW(TAG, "runtime config reset");
    return ESP_OK;
}
