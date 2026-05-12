#include "tinyblok_web.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "tinyblok_captive_html.h"
#include "tinyblok_config.h"
#include "tinyblok_setup_html.h"
#include "tinyblok_wifi.h"

static const char *TAG = "tinyblok_web";

static httpd_handle_t server;
static bool setup_mode;
static char last_error[160];

static esp_err_t json_begin(httpd_req_t *req, int status)
{
    char status_text[32];
    snprintf(status_text, sizeof(status_text), "%d", status);
    httpd_resp_set_status(req, status_text);
    httpd_resp_set_type(req, "application/json");
    return ESP_OK;
}

static esp_err_t json_send_error(httpd_req_t *req, int status, const char *message)
{
    char body[256];
    json_begin(req, status);
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message ? message : "error");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t json_send_message(httpd_req_t *req, int status, bool ok, const char *key, const char *message)
{
    char body[256];
    json_begin(req, status);
    snprintf(body, sizeof(body), "{\"ok\":%s,\"%s\":\"%s\"}", ok ? "true" : "false", key, message ? message : "");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t json_send_escaped_chunk(httpd_req_t *req, const char *s)
{
    char esc[8];
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        if (*p == '"' || *p == '\\')
        {
            esc[0] = '\\';
            esc[1] = (char)*p;
            ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, esc, 2), TAG, "send json escape");
        }
        else if (*p >= 0x20 && *p < 0x7f)
        {
            ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, (const char *)p, 1), TAG, "send json char");
        }
        else
        {
            snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
            ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, esc, 6), TAG, "send json unicode escape");
        }
    }
    return ESP_OK;
}

static esp_err_t send_setup_html(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)tinyblok_setup_html, tinyblok_setup_html_len);
}

static esp_err_t send_captive_html(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)tinyblok_captive_html, tinyblok_captive_html_len);
}

static esp_err_t send_status_json(httpd_req_t *req, const tinyblok_config_t *cfg)
{
    char ip[16];
    char body[384];
    tinyblok_wifi_get_ip_string(ip, sizeof(ip));
    int n = snprintf(body, sizeof(body),
                     "{\"firmware_version\":\"%s\",\"wifi_connected\":%s,\"ip_address\":\"%s\","
                     "\"configured\":%s,\"uptime_ms\":%llu,\"last_error\":\"%s\"}",
                     TINYBLOK_VERSION,
                     tinyblok_wifi_is_connected() ? "true" : "false",
                     ip,
                     cfg->configured ? "true" : "false",
                     (unsigned long long)(esp_timer_get_time() / 1000),
                     last_error);
    if (n < 0 || (size_t)n >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status overflow");
    json_begin(req, 200);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_settings_json(httpd_req_t *req, const tinyblok_config_t *cfg)
{
    char body[1024];
    int n = snprintf(body, sizeof(body),
                     "{\"device_name\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_password\":\"\","
                     "\"nats_host\":\"%s\",\"nats_port\":%u,\"nats_user\":\"%s\","
                     "\"nats_password\":\"\",\"nats_token\":\"\","
                     "\"nats_password_present\":%s,\"nats_token_present\":%s,"
                     "\"nats_auth\":\"%s\",\"nats_tls\":%s,\"nats_creds_present\":%s,"
                     "\"configured\":%s}",
                     cfg->device_name,
                     cfg->wifi_ssid,
                     cfg->nats_host,
                     cfg->nats_port,
                     cfg->nats_user,
                     cfg->nats_password[0] ? "true" : "false",
                     cfg->nats_token[0] ? "true" : "false",
                     cfg->nats_auth,
                     cfg->nats_tls ? "true" : "false",
                     cfg->nats_creds[0] ? "true" : "false",
                     cfg->configured ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(body))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "settings overflow");
    json_begin(req, 200);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t copy_form_value(char *dst, size_t dst_len, const char *src, size_t src_len, bool keep_empty)
{
    if (!keep_empty && src_len == 0)
        return ESP_OK;
    if (src_len >= dst_len)
        return ESP_ERR_INVALID_SIZE;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return ESP_OK;
}

static bool name_is(const char *name, size_t name_len, const char *lit)
{
    return strlen(lit) == name_len && memcmp(name, lit, name_len) == 0;
}

static esp_err_t apply_form_field(tinyblok_config_t *cfg, const char *name, size_t name_len,
                                  const char *value, size_t value_len)
{
    if (name_is(name, name_len, "device_name"))
        return copy_form_value(cfg->device_name, sizeof(cfg->device_name), value, value_len, true);
    if (name_is(name, name_len, "wifi_ssid"))
        return copy_form_value(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), value, value_len, true);
    if (name_is(name, name_len, "wifi_password"))
        return copy_form_value(cfg->wifi_password, sizeof(cfg->wifi_password), value, value_len, false);
    if (name_is(name, name_len, "nats_host"))
        return copy_form_value(cfg->nats_host, sizeof(cfg->nats_host), value, value_len, true);
    if (name_is(name, name_len, "nats_user"))
        return copy_form_value(cfg->nats_user, sizeof(cfg->nats_user), value, value_len, true);
    if (name_is(name, name_len, "nats_password"))
        return copy_form_value(cfg->nats_password, sizeof(cfg->nats_password), value, value_len, false);
    if (name_is(name, name_len, "nats_token"))
        return copy_form_value(cfg->nats_token, sizeof(cfg->nats_token), value, value_len, false);
    if (name_is(name, name_len, "nats_auth"))
        return copy_form_value(cfg->nats_auth, sizeof(cfg->nats_auth), value, value_len, true);
    if (name_is(name, name_len, "nats_creds_file"))
        return copy_form_value(cfg->nats_creds, sizeof(cfg->nats_creds), value, value_len, false);
    if (name_is(name, name_len, "nats_tls"))
    {
        cfg->nats_tls = value_len > 0 && !(value_len == 1 && value[0] == '0');
        return ESP_OK;
    }
    if (name_is(name, name_len, "nats_port"))
    {
        if (value_len == 0 || value_len >= 8)
            return ESP_ERR_INVALID_ARG;
        char tmp[8];
        memcpy(tmp, value, value_len);
        tmp[value_len] = '\0';
        char *end = NULL;
        unsigned long port = strtoul(tmp, &end, 10);
        if (end == tmp || *end != '\0' || port == 0 || port > 65535)
            return ESP_ERR_INVALID_ARG;
        cfg->nats_port = (uint16_t)port;
        return ESP_OK;
    }
    return ESP_OK;
}

static const char *memmem_bounded(const char *hay, size_t hay_len, const char *needle, size_t needle_len)
{
    if (needle_len == 0 || needle_len > hay_len)
        return NULL;
    for (size_t i = 0; i + needle_len <= hay_len; i++)
    {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    }
    return NULL;
}

static esp_err_t find_part_name(const char *headers, size_t headers_len,
                                const char **name_out, size_t *name_len_out)
{
    static const char key[] = " name=\"";
    const char *p = memmem_bounded(headers, headers_len, key, sizeof(key) - 1);
    if (!p)
        return ESP_ERR_INVALID_ARG;
    p += sizeof(key) - 1;
    size_t remaining = (size_t)(headers + headers_len - p);
    const char *q = memchr(p, '"', remaining);
    if (!q)
        return ESP_ERR_INVALID_ARG;
    *name_out = p;
    *name_len_out = (size_t)(q - p);
    return ESP_OK;
}

static esp_err_t apply_multipart_form(const char *body, size_t body_len,
                                      const char *boundary, tinyblok_config_t *cfg)
{
    char marker[96];
    int marker_n = snprintf(marker, sizeof(marker), "--%s", boundary);
    if (marker_n < 0 || (size_t)marker_n >= sizeof(marker))
        return ESP_ERR_INVALID_ARG;
    const size_t marker_len = (size_t)marker_n;

    const char *body_end = body + body_len;
    const char *part = memmem_bounded(body, body_len, marker, marker_len);
    if (!part)
        return ESP_ERR_INVALID_ARG;

    cfg->nats_tls = false;
    while (part)
    {
        part += marker_len;
        if (part + 2 > body_end)
            return ESP_ERR_INVALID_ARG;
        if (part[0] == '-' && part[1] == '-')
            break;
        if (part[0] == '\r' && part[1] == '\n')
            part += 2;

        static const char sep[] = "\r\n\r\n";
        const char *headers_end = memmem_bounded(part, (size_t)(body_end - part), sep, sizeof(sep) - 1);
        if (!headers_end)
            return ESP_ERR_INVALID_ARG;

        const char *name;
        size_t name_len;
        ESP_RETURN_ON_ERROR(find_part_name(part, (size_t)(headers_end - part), &name, &name_len),
                            TAG, "find part name");

        const char *value = headers_end + (sizeof(sep) - 1);
        const char *next = memmem_bounded(value, (size_t)(body_end - value), marker, marker_len);
        if (!next)
            return ESP_ERR_INVALID_ARG;
        const char *value_end = next;
        if (value_end >= value + 2 && value_end[-2] == '\r' && value_end[-1] == '\n')
            value_end -= 2;

        ESP_RETURN_ON_ERROR(apply_form_field(cfg, name, name_len, value, (size_t)(value_end - value)),
                            TAG, "apply form field");
        part = next;
    }

    if (strcmp(cfg->nats_auth, "creds") == 0)
        cfg->nats_tls = true;
    if (cfg->wifi_ssid[0] == '\0' || cfg->device_name[0] == '\0' || cfg->nats_host[0] == '\0')
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t apply_settings_form(httpd_req_t *req, const char *body, size_t body_len,
                                     tinyblok_config_t *cfg)
{
    char content_type[128];
    size_t content_type_len = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (content_type_len == 0 || content_type_len >= sizeof(content_type))
        return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)),
                        TAG, "get content-type");

    char *boundary = strstr(content_type, "boundary=");
    if (!boundary)
        return ESP_ERR_INVALID_ARG;
    boundary += strlen("boundary=");
    if (*boundary == '"')
    {
        boundary++;
        char *end = strchr(boundary, '"');
        if (end)
            *end = '\0';
    }
    return apply_multipart_form(body, body_len, boundary, cfg);
}

static esp_err_t read_request_body(httpd_req_t *req, char *buf, size_t cap, size_t *out_len)
{
    if (req->content_len == 0 || req->content_len > cap)
        return ESP_ERR_INVALID_SIZE;
    size_t off = 0;
    while (off < req->content_len)
    {
        int n = httpd_req_recv(req, buf + off, req->content_len - off);
        if (n <= 0)
            return ESP_FAIL;
        off += (size_t)n;
    }
    *out_len = off;
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    return send_setup_html(req);
}

static esp_err_t captive_get(httpd_req_t *req)
{
    /*
     * Captive-network assistants are short-lived and dislike redirect loops.
     * Serve a tiny local page for every probe/unknown URL; / has the full UI.
     */
    return send_captive_html(req);
}

static esp_err_t status_get(httpd_req_t *req)
{
    tinyblok_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    esp_err_t err = tinyblok_config_load(cfg);
    if (err != ESP_OK)
    {
        free(cfg);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    err = send_status_json(req, cfg);
    free(cfg);
    return err;
}

static esp_err_t settings_get(httpd_req_t *req)
{
    tinyblok_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    esp_err_t err = tinyblok_config_load(cfg);
    if (err != ESP_OK)
    {
        free(cfg);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    err = send_settings_json(req, cfg);
    free(cfg);
    return err;
}

static const char *wifi_auth_name(uint8_t authmode)
{
    switch ((wifi_auth_mode_t)authmode)
    {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa/wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2-enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2/wpa3";
    default:
        return "unknown";
    }
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    tinyblok_wifi_scan_result_t results[TINYBLOK_WIFI_SCAN_MAX];
    size_t count = 0;
    esp_err_t err = tinyblok_wifi_scan(results, TINYBLOK_WIFI_SCAN_MAX, &count);
    if (err != ESP_OK)
        return json_send_error(req, 503, esp_err_to_name(err));

    json_begin(req, 200);
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, "{\"networks\":[", HTTPD_RESP_USE_STRLEN),
                        TAG, "send scan begin");
    for (size_t i = 0; i < count; i++)
    {
        char prefix[96];
        snprintf(prefix, sizeof(prefix), "%s{\"ssid\":\"", i == 0 ? "" : ",");
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, prefix, HTTPD_RESP_USE_STRLEN), TAG, "send scan item");
        ESP_RETURN_ON_ERROR(json_send_escaped_chunk(req, results[i].ssid), TAG, "send scan ssid");
        char suffix[96];
        snprintf(suffix, sizeof(suffix), "\",\"rssi\":%d,\"auth\":\"%s\"}",
                 (int)results[i].rssi, wifi_auth_name(results[i].authmode));
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, suffix, HTTPD_RESP_USE_STRLEN), TAG, "send scan suffix");
    }
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN), TAG, "send scan end");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char *body = malloc(8192);
    if (!body)
        return json_send_error(req, 500, "no memory");
    size_t body_len = 0;
    esp_err_t err = read_request_body(req, body, 8192, &body_len);
    if (err != ESP_OK)
    {
        free(body);
        return json_send_error(req, 400, "invalid request body");
    }

    tinyblok_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
    {
        free(body);
        return json_send_error(req, 500, "no memory");
    }
    err = tinyblok_config_load(cfg);
    if (err == ESP_OK)
        err = apply_settings_form(req, body, body_len, cfg);
    free(body);
    if (err != ESP_OK)
    {
        snprintf(last_error, sizeof(last_error), "Invalid settings: %s", esp_err_to_name(err));
        free(cfg);
        return json_send_error(req, 400, last_error);
    }

    if (setup_mode)
    {
        cfg->configured = false;
        err = tinyblok_config_save(cfg);
        if (err == ESP_OK)
            err = tinyblok_wifi_connect_sta(cfg->wifi_ssid, cfg->wifi_password, 15000);
        if (err != ESP_OK)
        {
            snprintf(last_error, sizeof(last_error), "Wi-Fi connection failed: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "%s", last_error);
            free(cfg);
            return json_send_message(req, 409, false, "error", last_error);
        }
        cfg->configured = true;
        err = tinyblok_config_save(cfg);
        free(cfg);
        ESP_RETURN_ON_ERROR(err, TAG, "mark configured");
        last_error[0] = '\0';
        err = json_send_message(req, 200, true, "message", "Connected. Rebooting into normal mode.");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
        return err;
    }

    cfg->configured = true;
    err = tinyblok_config_save(cfg);
    free(cfg);
    if (err != ESP_OK)
        return json_send_error(req, 500, esp_err_to_name(err));
    last_error[0] = '\0';
    return json_send_message(req, 200, true, "message", "Settings saved.");
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    json_send_message(req, 200, true, "message", "Rebooting");
    xTaskCreate(reboot_task, "tinyblok_reboot", 2048, NULL, 1, NULL);
    return ESP_OK;
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    esp_err_t err = tinyblok_config_reset();
    (void)esp_wifi_restore();
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    json_send_message(req, 200, true, "message", "Factory reset complete. Rebooting");
    xTaskCreate(reboot_task, "tinyblok_reset", 2048, NULL, 1, NULL);
    return ESP_OK;
}

static esp_err_t register_handlers(void)
{
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    const httpd_uri_t settings_get_uri = {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get};
    const httpd_uri_t wifi_scan = {.uri = "/api/wifi-scan", .method = HTTP_GET, .handler = wifi_scan_get};
    const httpd_uri_t settings_post_uri = {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post};
    const httpd_uri_t reboot = {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post};
    const httpd_uri_t reset = {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_post};
    const httpd_uri_t captive_any = {.uri = "/*", .method = HTTP_GET, .handler = captive_get};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "register /");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &status), TAG, "register status");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings_get_uri), TAG, "register settings get");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &wifi_scan), TAG, "register wifi scan");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings_post_uri), TAG, "register settings post");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &reboot), TAG, "register reboot");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &reset), TAG, "register reset");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_any), TAG, "register captive wildcard");
    return ESP_OK;
}

static esp_err_t start_server(bool setup)
{
    setup_mode = setup;
    if (server)
        return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 9;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "start http server");
    ESP_RETURN_ON_ERROR(register_handlers(), TAG, "register handlers");
    ESP_LOGI(TAG, "%s web server started", setup ? "setup" : "LAN");
    return ESP_OK;
}

static void make_hostname(const char *device_name, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; device_name[i] != '\0' && j + 1 < out_len; i++)
    {
        unsigned char ch = (unsigned char)device_name[i];
        if (isalnum(ch))
            out[j++] = (char)tolower(ch);
        else if ((ch == '-' || ch == '_' || ch == ' ') && j > 0 && out[j - 1] != '-')
            out[j++] = '-';
    }
    while (j > 0 && out[j - 1] == '-')
        j--;
    if (j == 0)
        strlcpy(out, "tinyblok", out_len);
    else
        out[j] = '\0';
}

static esp_err_t start_mdns(void)
{
    tinyblok_config_t *cfg = calloc(1, sizeof(*cfg));
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_NO_MEM, TAG, "allocate config for mdns");
    esp_err_t err = tinyblok_config_load(cfg);
    if (err != ESP_OK)
    {
        free(cfg);
        ESP_RETURN_ON_ERROR(err, TAG, "load config for mdns");
    }
    char hostname[64];
    make_hostname(cfg->device_name, hostname, sizeof(hostname));
    err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        free(cfg);
        return err;
    }
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK)
    {
        free(cfg);
        ESP_RETURN_ON_ERROR(err, TAG, "set mdns hostname");
    }
    err = mdns_instance_name_set(cfg->device_name[0] ? cfg->device_name : "tinyblok");
    if (err != ESP_OK)
    {
        free(cfg);
        ESP_RETURN_ON_ERROR(err, TAG, "set mdns instance");
    }
    free(cfg);
    ESP_RETURN_ON_ERROR(mdns_service_add("tinyblok", "_http", "_tcp", 80, NULL, 0),
                        TAG, "add mdns http service");
    ESP_LOGI(TAG, "mDNS started: http://%s.local", hostname);
    return ESP_OK;
}

esp_err_t tinyblok_web_start_setup_portal(void)
{
    return start_server(true);
}

esp_err_t tinyblok_web_start_lan_server(void)
{
    ESP_RETURN_ON_ERROR(start_mdns(), TAG, "start mdns");
    return start_server(false);
}

esp_err_t tinyblok_web_stop(void)
{
    if (!server)
        return ESP_OK;
    esp_err_t err = httpd_stop(server);
    server = NULL;
    return err;
}
