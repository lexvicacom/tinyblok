#include <string.h>
#include "driver/temperature_sensor.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

extern void zig_main(void);

extern int tinyblok_nats_connect(void);
extern int tinyblok_nats_publish(const char *subject, const char *payload, size_t payload_len);

static const char *TAG = "tinyblok";

static temperature_sensor_handle_t tsens;

static EventGroupHandle_t wifi_events;
#define WIFI_GOT_IP_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "wifi disconnected, reconnecting");
        xEventGroupClearBits(wifi_events, WIFI_GOT_IP_BIT);
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_GOT_IP_BIT);
    }
}

static void wifi_connect_blocking(void)
{
    wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, CONFIG_TINYBLOK_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_TINYBLOK_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to ssid '%s'", CONFIG_TINYBLOK_WIFI_SSID);
    xEventGroupWaitBits(wifi_events, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

void app_main(void)
{
    // NVS required by esp_wifi for calibration data even when creds come from menuconfig.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    else
    {
        ESP_ERROR_CHECK(err);
    }

    wifi_connect_blocking();

    if (tinyblok_nats_connect() != 0)
    {
        ESP_LOGE(TAG, "nats connect failed; continuing without broker");
    }

    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    temperature_sensor_install(&cfg, &tsens);
    temperature_sensor_enable(tsens);
    zig_main();
}

float tinyblok_read_temp_c(void)
{
    float c = 0.0f;
    temperature_sensor_get_celsius(tsens, &c);
    return c;
}

uint32_t tinyblok_free_heap(void)
{
    return esp_get_free_heap_size();
}

uint64_t tinyblok_uptime_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

// Returns RSSI in dBm, or 0 if not associated (also returned mid-(re)connect).
int tinyblok_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
        return 0;
    return ap.rssi;
}
