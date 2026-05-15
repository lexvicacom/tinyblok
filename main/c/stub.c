#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include <stdlib.h>

#include "sdkconfig.h"

#include "app_events.h"
#include "display.h"
#include "tinyblok_config.h"
#include "tinyblok_patchbay.h"
#include "tinyblok_web.h"
#include "tinyblok_wifi.h"

extern int tinyblok_nats_connect(void);
extern void tinyblok_drivers_start(void);
extern void tinyblok_sources_init(void);

static const char *TAG = "tinyblok";

static portMUX_TYPE tx_ring_mux = portMUX_INITIALIZER_UNLOCKED;

void tinyblok_tx_ring_lock(void)
{
    portENTER_CRITICAL(&tx_ring_mux);
}

void tinyblok_tx_ring_unlock(void)
{
    portEXIT_CRITICAL(&tx_ring_mux);
}

void app_main(void)
{
    // NVS is required by esp_wifi for calibration data.
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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(tinyblok_config_init());
    tinyblok_events_init();
    tinyblok_display_start();

    tinyblok_config_t *cfg = calloc(1, sizeof(*cfg));
    ESP_ERROR_CHECK(cfg ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(tinyblok_config_load(cfg));
    if (!cfg->configured || cfg->wifi_ssid[0] == '\0')
    {
        free(cfg);
        ESP_LOGI(TAG, "runtime config missing; starting setup portal");
        tinyblok_display_setup_portal("TINYBLOK");
        ESP_ERROR_CHECK(tinyblok_wifi_start_setup_ap());
        ESP_ERROR_CHECK(tinyblok_web_start_setup_portal());
        for (;;)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_err_t wifi_err = tinyblok_wifi_connect_sta(cfg->wifi_ssid, cfg->wifi_password, 30000);
    free(cfg);
    if (wifi_err != ESP_OK)
    {
        ESP_LOGW(TAG, "saved Wi-Fi failed (%s); starting setup portal", esp_err_to_name(wifi_err));
        tinyblok_display_setup_portal("TINYBLOK");
        ESP_ERROR_CHECK(tinyblok_wifi_start_setup_ap());
        ESP_ERROR_CHECK(tinyblok_web_start_setup_portal());
        for (;;)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_ERROR_CHECK(tinyblok_wifi_stop_setup_ap());

    err = tinyblok_web_start_lan_server();
    if (err != ESP_OK)
        ESP_LOGW(TAG, "LAN web server failed: %s", esp_err_to_name(err));

    tinyblok_sources_init();
    if (tinyblok_patchbay_init() != 0)
    {
        ESP_LOGE(TAG, "patchbay init failed");
        abort();
    }
    tinyblok_drivers_start();

    if (tinyblok_nats_connect() != 0)
    {
        ESP_LOGE(TAG, "nats connect failed; continuing without broker");
    }

    tinyblok_patchbay_main();
}
