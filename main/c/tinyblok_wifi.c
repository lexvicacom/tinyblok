#include "tinyblok_wifi.h"

#include <string.h>

#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "tinyblok_wifi";

#define SETUP_AP_SSID "TINYBLOK"
#define SETUP_AP_IP_A 10
#define SETUP_AP_IP_B 42
#define SETUP_AP_IP_C 0
#define SETUP_AP_IP_D 1
#define SETUP_AP_URL "http://10.42.0.1/"

static char captive_portal_uri[] = SETUP_AP_URL;

#define WIFI_GOT_IP_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t wifi_events;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static bool wifi_inited;
static bool handlers_registered;
static bool connected;
static bool sta_connecting;
static bool setup_ap_started;
static TaskHandle_t dns_task_handle;
static char ip_string[16] = "0.0.0.0";
static char current_ssid[64] = "";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        connected = false;
        ip_string[0] = '\0';
        xEventGroupClearBits(wifi_events, WIFI_GOT_IP_BIT);
        tinyblok_display_wifi_disconnected();
        ESP_LOGW(TAG, "wifi disconnected");
        if (sta_connecting)
            xEventGroupSetBits(wifi_events, WIFI_FAIL_BIT);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(ip_string, sizeof(ip_string), IPSTR, IP2STR(&event->ip_info.ip));
        connected = true;
        sta_connecting = false;
        ESP_LOGI(TAG, "got ip: %s", ip_string);
        tinyblok_display_wifi_connected(current_ssid, &event->ip_info.ip);
        xEventGroupSetBits(wifi_events, WIFI_GOT_IP_BIT);
    }
}

static void dns_name_skip(const uint8_t *buf, size_t len, size_t *off)
{
    while (*off < len)
    {
        uint8_t c = buf[(*off)++];
        if (c == 0)
            return;
        if ((c & 0xC0) == 0xC0)
        {
            if (*off < len)
                (*off)++;
            return;
        }
        *off += c;
    }
}

static size_t dns_build_reply(uint8_t *buf, size_t len)
{
    if (len < 12)
        return 0;
    uint16_t flags = ((uint16_t)buf[2] << 8) | buf[3];
    uint16_t qd = ((uint16_t)buf[4] << 8) | buf[5];
    if ((flags & 0x8000) || qd == 0)
        return 0;

    size_t off = 12;
    dns_name_skip(buf, len, &off);
    if (off + 4 > len)
        return 0;
    size_t question_end = off + 4;
    if (question_end + 16 > 512)
        return 0;

    buf[2] = 0x81;
    buf[3] = 0x80;
    buf[6] = 0x00;
    buf[7] = 0x01;
    buf[8] = 0x00;
    buf[9] = 0x00;
    buf[10] = 0x00;
    buf[11] = 0x00;

    off = question_end;
    buf[off++] = 0xC0;
    buf[off++] = 0x0C;
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    buf[off++] = 0x00;
    buf[off++] = 0x00;
    buf[off++] = 0x00;
    buf[off++] = 0x3C;
    buf[off++] = 0x00;
    buf[off++] = 0x04;
    buf[off++] = SETUP_AP_IP_A;
    buf[off++] = SETUP_AP_IP_B;
    buf[off++] = SETUP_AP_IP_C;
    buf[off++] = SETUP_AP_IP_D;
    return off;
}

static void dns_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0)
    {
        ESP_LOGW(TAG, "dns socket failed");
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ESP_LOGW(TAG, "dns bind failed");
        close(fd);
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "setup DNS responder started: captive queries -> " SETUP_AP_URL);
    for (;;)
    {
        uint8_t buf[512];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0)
            continue;
        size_t out_len = dns_build_reply(buf, (size_t)n);
        if (out_len > 0)
            (void)sendto(fd, buf, out_len, 0, (struct sockaddr *)&from, from_len);
    }
}

static void start_dns_responder(void)
{
    if (dns_task_handle)
        return;
    BaseType_t ok = xTaskCreate(dns_task, "tinyblok_dns", 3072, NULL, 4, &dns_task_handle);
    if (ok != pdPASS)
        ESP_LOGW(TAG, "could not start setup DNS responder");
}

static esp_err_t ensure_wifi(void)
{
    if (!wifi_events)
    {
        wifi_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(wifi_events, ESP_ERR_NO_MEM, TAG, "create wifi event group");
    }
    if (!sta_netif)
    {
        sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(sta_netif, ESP_ERR_NO_MEM, TAG, "create STA netif");
    }
    if (!ap_netif)
    {
        ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(ap_netif, ESP_ERR_NO_MEM, TAG, "create AP netif");
        esp_netif_ip_info_t ip = {0};
        esp_netif_set_ip4_addr(&ip.ip, SETUP_AP_IP_A, SETUP_AP_IP_B, SETUP_AP_IP_C, SETUP_AP_IP_D);
        esp_netif_set_ip4_addr(&ip.gw, SETUP_AP_IP_A, SETUP_AP_IP_B, SETUP_AP_IP_C, SETUP_AP_IP_D);
        esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
        esp_err_t dhcp_err = esp_netif_dhcps_stop(ap_netif);
        if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
            ESP_LOGW(TAG, "stop setup DHCP before IP config: %s", esp_err_to_name(dhcp_err));
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ip), TAG, "set setup AP IP");
    }
    if (!wifi_inited)
    {
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
        wifi_inited = true;
    }
    if (!handlers_registered)
    {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                            TAG, "register wifi handler");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                            TAG, "register ip handler");
        handlers_registered = true;
    }
    return ESP_OK;
}

static esp_err_t configure_setup_dhcp(void)
{
    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
        ESP_LOGW(TAG, "stop setup DHCP server before option update: %s", esp_err_to_name(err));

    uint8_t offer_dns = 1;
    err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns, sizeof(offer_dns));
    if (err != ESP_OK)
        ESP_LOGW(TAG, "enable setup DHCP DNS option: %s", esp_err_to_name(err));

    err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                 captive_portal_uri, strlen(captive_portal_uri));
    if (err != ESP_OK)
        ESP_LOGW(TAG, "set setup DHCP captive portal URI: %s", esp_err_to_name(err));

    uint8_t offer_router = 1;
    err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                 &offer_router, sizeof(offer_router));
    if (err != ESP_OK)
        ESP_LOGW(TAG, "enable setup DHCP router option: %s", esp_err_to_name(err));

    return ESP_OK;
}

esp_err_t tinyblok_wifi_start_setup_ap(void)
{
    ESP_RETURN_ON_ERROR(ensure_wifi(), TAG, "ensure wifi");
    ESP_RETURN_ON_ERROR(configure_setup_dhcp(), TAG, "configure setup DHCP");

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, SETUP_AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(SETUP_AP_SSID);
    strlcpy((char *)ap_cfg.ap.password, CONFIG_TINYBLOK_SETUP_AP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    if (strlen(CONFIG_TINYBLOK_SETUP_AP_PASSWORD) >= 8)
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    else
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config");
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
        ESP_RETURN_ON_ERROR(err, TAG, "start wifi");
    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        ESP_LOGW(TAG, "start setup DHCP server: %s", esp_err_to_name(err));

    esp_netif_ip_info_t ip;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap_netif, &ip), TAG, "get AP IP");
    ESP_LOGI(TAG, "setup AP '%s' at " IPSTR, SETUP_AP_SSID, IP2STR(&ip.ip));
    setup_ap_started = true;
    start_dns_responder();
    return ESP_OK;
}

esp_err_t tinyblok_wifi_stop_setup_ap(void)
{
    if (!wifi_inited || !setup_ap_started)
        return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "switch wifi to STA mode");
    setup_ap_started = false;
    return ESP_OK;
}

esp_err_t tinyblok_wifi_connect_sta(const char *ssid, const char *password, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "ssid is required");
    ESP_RETURN_ON_ERROR(ensure_wifi(), TAG, "ensure wifi");

    xEventGroupClearBits(wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);
    connected = false;
    sta_connecting = true;
    strlcpy(current_ssid, ssid, sizeof(current_ssid));

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    if (password)
        strlcpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(setup_ap_started ? WIFI_MODE_APSTA : WIFI_MODE_STA), TAG, "set wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA config");
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
        ESP_RETURN_ON_ERROR(err, TAG, "start wifi");

    ESP_LOGI(TAG, "connecting to ssid '%s'", ssid);
    tinyblok_display_wifi_connecting(ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
    {
        sta_connecting = false;
        ESP_RETURN_ON_ERROR(err, TAG, "wifi connect");
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    sta_connecting = false;
    if ((bits & WIFI_GOT_IP_BIT) == 0)
    {
        (void)esp_wifi_disconnect();
        if (bits & WIFI_FAIL_BIT)
        {
            ESP_LOGW(TAG, "wifi connect failed");
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "wifi connect timed out after %u ms", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool tinyblok_wifi_is_connected(void)
{
    return connected;
}

esp_err_t tinyblok_wifi_get_ip_string(char *buf, size_t len)
{
    ESP_RETURN_ON_FALSE(buf && len > 0, ESP_ERR_INVALID_ARG, TAG, "bad output buffer");
    strlcpy(buf, connected && ip_string[0] != '\0' ? ip_string : "0.0.0.0", len);
    return ESP_OK;
}
