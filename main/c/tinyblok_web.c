#include "tinyblok_web.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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
"<h1>tinyblok setup</h1><p>Join the tinyblok-setup Wi-Fi network, open http://tinyblok.setup, then save these settings. No phone app is required.</p><div id=\"msg\" class=\"msg\"></div>"
"<form id=\"f\" class=\"grid\">"
"<label>Wi-Fi SSID<input name=\"wifi_ssid\" maxlength=\"63\" required></label>"
"<label>Wi-Fi password<input name=\"wifi_password\" type=\"password\" maxlength=\"127\" autocomplete=\"new-password\"></label>"
"<label>Device name<input name=\"device_name\" maxlength=\"63\" required></label>"
"<div class=\"row\"><label>NATS host<input name=\"nats_host\" maxlength=\"127\" required></label>"
"<label>NATS port<input name=\"nats_port\" type=\"number\" min=\"1\" max=\"65535\" required></label></div>"
"<div class=\"row\"><label>NATS auth<select name=\"nats_auth\"><option value=\"none\">None</option><option value=\"userpass\">Username + password</option><option value=\"token\">Token</option><option value=\"creds\">.creds file</option></select></label>"
"<label class=\"check\"><input name=\"nats_tls\" type=\"checkbox\">Use TLS</label></div>"
"<div class=\"row\" id=\"userpassrow\"><label>NATS username<input name=\"nats_user\" maxlength=\"63\"></label>"
"<label>NATS password<input name=\"nats_password\" type=\"password\" maxlength=\"127\" autocomplete=\"new-password\"></label></div>"
"<label id=\"tokenrow\">NATS token<input name=\"nats_token\" type=\"password\" maxlength=\"255\" autocomplete=\"new-password\"></label>"
"<label id=\"credsrow\">NATS .creds file<input name=\"nats_creds_file\" type=\"file\" accept=\".creds,text/plain\"></label>"
"<label>NATS base topic<input name=\"nats_base_topic\" maxlength=\"127\"></label>"
"<div class=\"row\"><label>Sample interval (ms)<input name=\"sample_interval_ms\" type=\"number\" min=\"100\" required></label>"
"<label>Timezone<input name=\"timezone\" maxlength=\"63\"></label></div>"
"<div class=\"row\"><label>Display brightness<input name=\"display_brightness\" type=\"number\" min=\"0\" max=\"255\" required></label>"
"<label class=\"check\"><input name=\"display_enabled\" type=\"checkbox\">Display enabled</label></div>"
"<div class=\"actions\"><button>Save and connect</button><button class=\"danger\" type=\"button\" id=\"reset\">Factory reset</button></div>"
"</form></main><script>"
"const $=s=>document.querySelector(s),msg=$('#msg'),form=$('#f');"
"function authChanged(){let a=form.elements.nats_auth.value,creds=a==='creds';form.elements.nats_tls.checked=creds||form.elements.nats_tls.checked;form.elements.nats_tls.disabled=creds;$('#credsrow').style.display=creds?'grid':'none';$('#userpassrow').style.display=a==='userpass'?'grid':'none';$('#tokenrow').style.display=a==='token'?'grid':'none'}"
"function readFile(file){return new Promise((ok,fail)=>{if(!file)return ok('');let r=new FileReader();r.onload=()=>ok(String(r.result));r.onerror=()=>fail(r.error);r.readAsText(file)})}"
"function show(t,c){msg.textContent=t||'';msg.className='msg '+(c||'')}"
"async function load(){let [s,r]=await Promise.all([fetch('/api/settings'),fetch('/api/status')]);let j=await s.json(),st=await r.json();"
"for(const [k,v] of Object.entries(j)){let e=form.elements[k];if(!e)continue;if(e.type==='checkbox')e.checked=!!v;else e.value=(v==null?'':v)}"
"authChanged();if(st.last_error)show(st.last_error,'err')}"
"form.onchange=e=>{if(e.target.name==='nats_auth')authChanged()};"
"form.onsubmit=async e=>{e.preventDefault();show('Saving...');let o={};for(const el of form.elements){if(!el.name||el.type==='file')continue;o[el.name]=el.type==='checkbox'?el.checked:(el.type==='number'?Number(el.value):el.value)}"
"o.nats_tls=form.elements.nats_auth.value==='creds'||form.elements.nats_tls.checked;o.nats_creds=await readFile(form.elements.nats_creds_file.files[0]);"
"let r=await fetch('/api/settings',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(o)});let j=await r.json();"
"show(j.message||j.error,(r.ok&&j.ok)?'ok':'err')};"
"$('#reset').onclick=async()=>{if(confirm('Factory reset tinyblok settings?')){await fetch('/api/factory-reset',{method:'POST'});show('Rebooting...','ok')}};"
"load().catch(e=>show(String(e),'err'));</script></body></html>";

static esp_err_t send_json(httpd_req_t *req, cJSON *json, int status)
{
    char status_text[32];
    snprintf(status_text, sizeof(status_text), "%d", status);
    httpd_resp_set_status(req, status_text);
    httpd_resp_set_type(req, "application/json");
    char *body = cJSON_PrintUnformatted(json);
    if (!body)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static esp_err_t send_error_json(httpd_req_t *req, int status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    esp_err_t err = send_json(req, root, status);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_message_json(httpd_req_t *req, int status, bool ok, const char *key, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, key, message);
    esp_err_t err = send_json(req, root, status);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_status_json(httpd_req_t *req, const tinyblok_config_t *cfg)
{
    char ip[16];
    tinyblok_wifi_get_ip_string(ip, sizeof(ip));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "firmware_version", TINYBLOK_VERSION);
    cJSON_AddBoolToObject(root, "wifi_connected", tinyblok_wifi_is_connected());
    cJSON_AddStringToObject(root, "ip_address", ip);
    cJSON_AddBoolToObject(root, "configured", cfg->configured);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddStringToObject(root, "last_error", last_error);
    esp_err_t err = send_json(req, root, 200);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_settings_json(httpd_req_t *req, const tinyblok_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", cfg->device_name);
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", "");
    cJSON_AddStringToObject(root, "nats_host", cfg->nats_host);
    cJSON_AddNumberToObject(root, "nats_port", cfg->nats_port);
    cJSON_AddStringToObject(root, "nats_user", cfg->nats_user);
    cJSON_AddStringToObject(root, "nats_password", "");
    cJSON_AddStringToObject(root, "nats_token", "");
    cJSON_AddBoolToObject(root, "nats_password_present", cfg->nats_password[0] != '\0');
    cJSON_AddBoolToObject(root, "nats_token_present", cfg->nats_token[0] != '\0');
    cJSON_AddStringToObject(root, "nats_base_topic", cfg->nats_base_topic);
    cJSON_AddStringToObject(root, "nats_auth", cfg->nats_auth);
    cJSON_AddBoolToObject(root, "nats_tls", cfg->nats_tls);
    cJSON_AddBoolToObject(root, "nats_creds_present", cfg->nats_creds[0] != '\0');
    cJSON_AddBoolToObject(root, "display_enabled", cfg->display_enabled);
    cJSON_AddNumberToObject(root, "display_brightness", cfg->display_brightness);
    cJSON_AddNumberToObject(root, "sample_interval_ms", cfg->sample_interval_ms);
    cJSON_AddStringToObject(root, "timezone", cfg->timezone);
    cJSON_AddBoolToObject(root, "configured", cfg->configured);
    esp_err_t err = send_json(req, root, 200);
    cJSON_Delete(root);
    return err;
}

static esp_err_t copy_json_string(cJSON *root, const char *key, char *dst, size_t dst_len, bool keep_empty)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item)
        return ESP_OK;
    if (!cJSON_IsString(item) || !item->valuestring)
        return ESP_ERR_INVALID_ARG;
    if (!keep_empty && item->valuestring[0] == '\0')
        return ESP_OK;
    size_t len = strnlen(item->valuestring, dst_len);
    if (len >= dst_len)
        return ESP_ERR_INVALID_SIZE;
    memcpy(dst, item->valuestring, len);
    dst[len] = '\0';
    return ESP_OK;
}

static esp_err_t apply_settings_json(cJSON *root, tinyblok_config_t *cfg)
{
    ESP_RETURN_ON_ERROR(copy_json_string(root, "device_name", cfg->device_name, sizeof(cfg->device_name), true),
                        TAG, "device_name");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid), true),
                        TAG, "wifi_ssid");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "wifi_password", cfg->wifi_password, sizeof(cfg->wifi_password), false),
                        TAG, "wifi_password");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "nats_host", cfg->nats_host, sizeof(cfg->nats_host), true),
                        TAG, "nats_host");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "nats_user", cfg->nats_user, sizeof(cfg->nats_user), true),
                        TAG, "nats_user");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "nats_password", cfg->nats_password, sizeof(cfg->nats_password), false),
                        TAG, "nats_password");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "nats_token", cfg->nats_token, sizeof(cfg->nats_token), false),
                        TAG, "nats_token");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "nats_base_topic", cfg->nats_base_topic, sizeof(cfg->nats_base_topic), true),
                        TAG, "nats_base_topic");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "nats_auth", cfg->nats_auth, sizeof(cfg->nats_auth), true),
                        TAG, "nats_auth");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "nats_creds", cfg->nats_creds, sizeof(cfg->nats_creds), false),
                        TAG, "nats_creds");
    ESP_RETURN_ON_ERROR(copy_json_string(root, "timezone", cfg->timezone, sizeof(cfg->timezone), true),
                        TAG, "timezone");

    cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "nats_port");
    if (port)
    {
        if (!cJSON_IsNumber(port) || port->valuedouble < 1 || port->valuedouble > 65535)
            return ESP_ERR_INVALID_ARG;
        cfg->nats_port = (uint16_t)port->valueint;
    }
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "display_enabled");
    if (enabled)
    {
        if (!cJSON_IsBool(enabled))
            return ESP_ERR_INVALID_ARG;
        cfg->display_enabled = cJSON_IsTrue(enabled);
    }
    cJSON *tls = cJSON_GetObjectItemCaseSensitive(root, "nats_tls");
    if (tls)
    {
        if (!cJSON_IsBool(tls))
            return ESP_ERR_INVALID_ARG;
        cfg->nats_tls = cJSON_IsTrue(tls);
    }
    if (strcmp(cfg->nats_auth, "creds") == 0)
        cfg->nats_tls = true;
    cJSON *brightness = cJSON_GetObjectItemCaseSensitive(root, "display_brightness");
    if (brightness)
    {
        if (!cJSON_IsNumber(brightness) || brightness->valuedouble < 0 || brightness->valuedouble > 255)
            return ESP_ERR_INVALID_ARG;
        cfg->display_brightness = (uint8_t)brightness->valueint;
    }
    cJSON *sample = cJSON_GetObjectItemCaseSensitive(root, "sample_interval_ms");
    if (sample)
    {
        if (!cJSON_IsNumber(sample) || sample->valuedouble < 100 || sample->valuedouble > UINT32_MAX)
            return ESP_ERR_INVALID_ARG;
        cfg->sample_interval_ms = (uint32_t)sample->valuedouble;
    }
    if (cfg->wifi_ssid[0] == '\0' || cfg->device_name[0] == '\0' || cfg->nats_host[0] == '\0')
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
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
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, setup_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    tinyblok_config_t cfg;
    esp_err_t err = tinyblok_config_load(&cfg);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return send_status_json(req, &cfg);
}

static esp_err_t settings_get(httpd_req_t *req)
{
    tinyblok_config_t cfg;
    esp_err_t err = tinyblok_config_load(&cfg);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return send_settings_json(req, &cfg);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char body[6144];
    esp_err_t err = read_request_body(req, body, sizeof(body));
    if (err != ESP_OK)
        return send_error_json(req, 400, "invalid request body");

    cJSON *json = cJSON_Parse(body);
    if (!json)
        return send_error_json(req, 400, "invalid json");

    tinyblok_config_t cfg;
    err = tinyblok_config_load(&cfg);
    if (err == ESP_OK)
        err = apply_settings_json(json, &cfg);
    cJSON_Delete(json);
    if (err != ESP_OK)
    {
        snprintf(last_error, sizeof(last_error), "Invalid settings: %s", esp_err_to_name(err));
        return send_error_json(req, 400, last_error);
    }

    if (setup_mode)
    {
        cfg.configured = false;
        err = tinyblok_config_save(&cfg);
        if (err == ESP_OK)
            err = tinyblok_wifi_connect_sta(cfg.wifi_ssid, cfg.wifi_password, 15000);
        if (err != ESP_OK)
        {
            snprintf(last_error, sizeof(last_error), "Wi-Fi connection failed: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "%s", last_error);
            return send_message_json(req, 409, false, "error", last_error);
        }
        cfg.configured = true;
        ESP_RETURN_ON_ERROR(tinyblok_config_save(&cfg), TAG, "mark configured");
        last_error[0] = '\0';
        err = send_message_json(req, 200, true, "message", "Connected. Rebooting into normal mode.");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
        return err;
    }

    cfg.configured = true;
    err = tinyblok_config_save(&cfg);
    if (err != ESP_OK)
        return send_error_json(req, 500, esp_err_to_name(err));
    last_error[0] = '\0';
    return send_message_json(req, 200, true, "message", "Settings saved.");
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Rebooting\"}");
    xTaskCreate(reboot_task, "tinyblok_reboot", 2048, NULL, 1, NULL);
    return ESP_OK;
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    esp_err_t err = tinyblok_config_reset();
    (void)esp_wifi_restore();
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Factory reset complete. Rebooting\"}");
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
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "register /");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &status), TAG, "register status");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings_get_uri), TAG, "register settings get");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings_post_uri), TAG, "register settings post");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &reboot), TAG, "register reboot");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &reset), TAG, "register reset");
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
    tinyblok_config_t cfg;
    ESP_RETURN_ON_ERROR(tinyblok_config_load(&cfg), TAG, "load config for mdns");
    char hostname[64];
    make_hostname(cfg.device_name, hostname, sizeof(hostname));
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;
    ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "set mdns hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(cfg.device_name[0] ? cfg.device_name : "tinyblok"), TAG, "set mdns instance");
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
