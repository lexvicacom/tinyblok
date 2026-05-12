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
#include "tinyblok_config.h"
#include "tinyblok_wifi.h"

static const char *TAG = "tinyblok_web";

static httpd_handle_t server;
static bool setup_mode;
static char last_error[160];

static const char setup_html[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>tinyblok setup</title><style>"
":root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#17201a;background:#f7f7f4}"
"body{margin:0;padding:20px;background:#101813;color:#e8efe7;background-image:linear-gradient(rgba(98,141,106,.13) 1px,transparent 1px),linear-gradient(90deg,rgba(98,141,106,.13) 1px,transparent 1px);background-size:28px 28px}"
".wrap{max-width:760px;margin:0 auto}h1{font-size:2rem;margin:0 0 4px;letter-spacing:0;color:#9ff0a8}"
"p{color:#b6c5b5}.grid{display:grid;gap:12px;background:#f6f4ea;color:#17201a;border:1px solid #47624d;border-radius:8px;padding:18px;box-shadow:0 18px 50px rgba(0,0,0,.35)}"
"label{display:grid;gap:5px;font-weight:650}"
"input,select,button{font:inherit;border:1px solid #aeb8ad;border-radius:6px;padding:11px;background:#fffef8;color:#17201a}"
"input:focus,select:focus{outline:2px solid #76c96e;border-color:#27663a}"
"input[type=checkbox]{width:22px;height:22px}.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
".check{display:flex;align-items:center;gap:10px}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}"
"button{background:#1f7a3d;color:white;border-color:#1f7a3d;font-weight:700}.danger{background:#9b2d20;border-color:#9b2d20}"
".msg{min-height:1.5em;margin:12px 0;font-weight:650}.err{color:#ffb0a3}.ok{color:#9ff0a8}"
"@media(max-width:620px){.row{grid-template-columns:1fr}body{padding:14px}}</style></head><body><main class=\"wrap\">"
"<h1>tinyblok setup</h1><p>Enter the Wi-Fi and NATS details for this device, then save to connect.</p><div id=\"msg\" class=\"msg\"></div>"
"<form id=\"f\" class=\"grid\" method=\"post\" action=\"/api/settings\" enctype=\"multipart/form-data\">"
"<label>Wi-Fi SSID<input name=\"wifi_ssid\" maxlength=\"63\" required></label>"
"<label>Wi-Fi password<input name=\"wifi_password\" type=\"password\" maxlength=\"127\" autocomplete=\"new-password\"></label>"
"<label>Device name<input name=\"device_name\" maxlength=\"63\" required></label>"
"<div class=\"row\"><label>NATS host<input name=\"nats_host\" maxlength=\"127\" required></label>"
"<label>NATS port<input name=\"nats_port\" type=\"number\" min=\"1\" max=\"65535\" required></label></div>"
"<div class=\"row\"><label>NATS auth<select name=\"nats_auth\"><option value=\"none\">None</option><option value=\"userpass\">Username + password</option><option value=\"token\">Token</option><option value=\"creds\">.creds file</option></select></label>"
"<label class=\"check\"><input name=\"nats_tls\" value=\"1\" type=\"checkbox\">Use TLS</label></div>"
"<div class=\"row\" id=\"userpassrow\"><label>NATS username<input name=\"nats_user\" maxlength=\"63\"></label>"
"<label>NATS password<input name=\"nats_password\" type=\"password\" maxlength=\"127\" autocomplete=\"new-password\"></label></div>"
"<label id=\"tokenrow\">NATS token<input name=\"nats_token\" type=\"password\" maxlength=\"255\" autocomplete=\"new-password\"></label>"
"<label id=\"credsrow\">NATS .creds file<input name=\"nats_creds_file\" type=\"file\" accept=\".creds,text/plain\"></label>"
"<div class=\"actions\"><button>Save and connect</button><button class=\"danger\" type=\"button\" id=\"reset\">Factory reset</button></div>"
"</form></main><script>"
"const $=s=>document.querySelector(s),msg=$('#msg'),form=$('#f');"
"function show(t,c){msg.textContent=t||'';msg.className='msg '+(c||'')}"
"function authChanged(){let a=form.elements.nats_auth.value,creds=a==='creds';form.elements.nats_tls.checked=creds||form.elements.nats_tls.checked;form.elements.nats_tls.disabled=creds;$('#credsrow').style.display=creds?'grid':'none';$('#userpassrow').style.display=a==='userpass'?'grid':'none';$('#tokenrow').style.display=a==='token'?'grid':'none'}"
"async function load(){let [s,r]=await Promise.all([fetch('/api/settings'),fetch('/api/status')]);let j=await s.json(),st=await r.json();for(const [k,v] of Object.entries(j)){let e=form.elements[k];if(!e)continue;if(e.type==='checkbox')e.checked=!!v;else e.value=(v==null?'':v)}authChanged();if(st.last_error)show(st.last_error,'err')}"
"form.onchange=e=>{if(e.target.name==='nats_auth')authChanged()};"
"form.onsubmit=async e=>{e.preventDefault();show('Saving...');let d=new FormData(form);if(form.elements.nats_auth.value==='creds')d.set('nats_tls','1');let r=await fetch('/api/settings',{method:'POST',body:d});let j=await r.json();show(j.message||j.error,(r.ok&&j.ok)?'ok':'err')};"
"$('#reset').onclick=async()=>{if(confirm('Factory reset tinyblok settings?')){await fetch('/api/factory-reset',{method:'POST'});show('Rebooting...','ok')}};"
"load().catch(e=>show(String(e),'err'));</script></body></html>";

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

static esp_err_t send_status_json(httpd_req_t *req, const tinyblok_config_t *cfg)
{
    char ip[16];
    char body[384];
    tinyblok_wifi_get_ip_string(ip, sizeof(ip));
    json_begin(req, 200);
    snprintf(body, sizeof(body),
             "{\"firmware_version\":\"%s\",\"wifi_connected\":%s,\"ip_address\":\"%s\","
             "\"configured\":%s,\"uptime_ms\":%llu,\"last_error\":\"%s\"}",
             TINYBLOK_VERSION,
             tinyblok_wifi_is_connected() ? "true" : "false",
             ip,
             cfg->configured ? "true" : "false",
             (unsigned long long)(esp_timer_get_time() / 1000),
             last_error);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_settings_json(httpd_req_t *req, const tinyblok_config_t *cfg)
{
    char body[1024];
    json_begin(req, 200);
    snprintf(body, sizeof(body),
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
    return httpd_resp_sendstr(req, body);
}

static esp_err_t copy_form_string(char *dst, size_t dst_len, const char *src, bool keep_empty)
{
    if (!src)
        return ESP_OK;
    if (!keep_empty && src[0] == '\0')
        return ESP_OK;
    size_t len = strnlen(src, dst_len);
    if (len >= dst_len)
        return ESP_ERR_INVALID_SIZE;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return ESP_OK;
}

static esp_err_t apply_form_field(tinyblok_config_t *cfg, const char *name, const char *value)
{
    if (strcmp(name, "device_name") == 0)
        return copy_form_string(cfg->device_name, sizeof(cfg->device_name), value, true);
    if (strcmp(name, "wifi_ssid") == 0)
        return copy_form_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), value, true);
    if (strcmp(name, "wifi_password") == 0)
        return copy_form_string(cfg->wifi_password, sizeof(cfg->wifi_password), value, false);
    if (strcmp(name, "nats_host") == 0)
        return copy_form_string(cfg->nats_host, sizeof(cfg->nats_host), value, true);
    if (strcmp(name, "nats_user") == 0)
        return copy_form_string(cfg->nats_user, sizeof(cfg->nats_user), value, true);
    if (strcmp(name, "nats_password") == 0)
        return copy_form_string(cfg->nats_password, sizeof(cfg->nats_password), value, false);
    if (strcmp(name, "nats_token") == 0)
        return copy_form_string(cfg->nats_token, sizeof(cfg->nats_token), value, false);
    if (strcmp(name, "nats_auth") == 0)
        return copy_form_string(cfg->nats_auth, sizeof(cfg->nats_auth), value, true);
    if (strcmp(name, "nats_creds_file") == 0)
        return copy_form_string(cfg->nats_creds, sizeof(cfg->nats_creds), value, false);
    if (strcmp(name, "nats_tls") == 0)
    {
        cfg->nats_tls = value && value[0] != '\0' && strcmp(value, "0") != 0;
        return ESP_OK;
    }
    if (strcmp(name, "nats_port") == 0)
    {
        char *end = NULL;
        unsigned long port = strtoul(value, &end, 10);
        if (!value[0] || (end && *end) || port == 0 || port > 65535)
            return ESP_ERR_INVALID_ARG;
        cfg->nats_port = (uint16_t)port;
        return ESP_OK;
    }
    return ESP_OK;
}

static char *find_header_param(char *headers, const char *headers_end, const char *param)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "%s=\"", param);
    for (char *p = strstr(headers, needle); p && p < headers_end; p = strstr(p + 1, needle))
        return p + strlen(needle);
    return NULL;
}

static esp_err_t apply_multipart_form(char *body, const char *boundary, tinyblok_config_t *cfg)
{
    char marker[96];
    snprintf(marker, sizeof(marker), "--%s", boundary);
    const size_t marker_len = strlen(marker);
    char *part = strstr(body, marker);
    if (!part)
        return ESP_ERR_INVALID_ARG;

    cfg->nats_tls = false;
    while (part)
    {
        part += marker_len;
        if (strncmp(part, "--", 2) == 0)
            break;
        if (strncmp(part, "\r\n", 2) == 0)
            part += 2;

        char *headers_end = strstr(part, "\r\n\r\n");
        if (!headers_end)
            return ESP_ERR_INVALID_ARG;
        char *name_start = find_header_param(part, headers_end, "name");
        if (!name_start)
            return ESP_ERR_INVALID_ARG;
        char *name_end = strchr(name_start, '"');
        if (!name_end || name_end > headers_end)
            return ESP_ERR_INVALID_ARG;
        *name_end = '\0';

        char *value = headers_end + 4;
        char *next = strstr(value, marker);
        if (!next)
            return ESP_ERR_INVALID_ARG;
        char *value_end = next;
        if (value_end >= value + 2 && value_end[-2] == '\r' && value_end[-1] == '\n')
            value_end -= 2;
        *value_end = '\0';

        ESP_RETURN_ON_ERROR(apply_form_field(cfg, name_start, value), TAG, "apply form field");
        part = next;
    }

    if (strcmp(cfg->nats_auth, "creds") == 0)
        cfg->nats_tls = true;
    if (cfg->wifi_ssid[0] == '\0' || cfg->device_name[0] == '\0' || cfg->nats_host[0] == '\0')
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t apply_settings_form(httpd_req_t *req, char *body, tinyblok_config_t *cfg)
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
    return apply_multipart_form(body, boundary, cfg);
}

static esp_err_t read_request_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap)
        return ESP_ERR_INVALID_SIZE;
    size_t off = 0;
    while (off < req->content_len)
    {
        int n = httpd_req_recv(req, buf + off, req->content_len - off);
        if (n <= 0)
            return ESP_FAIL;
        off += (size_t)n;
    }
    buf[off] = '\0';
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, setup_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_get(httpd_req_t *req)
{
    /*
     * Captive-network assistants are short-lived and dislike redirect loops.
     * Serve the same local setup page directly for every probe/unknown URL.
     */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, setup_html, HTTPD_RESP_USE_STRLEN);
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

static esp_err_t settings_post(httpd_req_t *req)
{
    char *body = malloc(8192);
    if (!body)
        return json_send_error(req, 500, "no memory");
    esp_err_t err = read_request_body(req, body, 8192);
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
        err = apply_settings_form(req, body, cfg);
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
    const httpd_uri_t settings_post_uri = {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post};
    const httpd_uri_t reboot = {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post};
    const httpd_uri_t reset = {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_post};
    const httpd_uri_t captive_any = {.uri = "/*", .method = HTTP_GET, .handler = captive_get};
    const httpd_uri_t probes[] = {
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_get},
        {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = captive_get},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_get},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = captive_get},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_get},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_get},
        {.uri = "/fwlink", .method = HTTP_GET, .handler = captive_get},
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "register /");
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &probes[i]), TAG, "register captive probe");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &status), TAG, "register status");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings_get_uri), TAG, "register settings get");
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
    cfg.max_uri_handlers = 16;
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
    mdns_service_add("tinyblok", "_http", "_tcp", 80, NULL, 0);
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
