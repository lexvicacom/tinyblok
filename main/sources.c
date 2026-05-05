// Pump data sources. Each function here is named in patchbay.edn as a
// `:from` symbol and called from a generated pump in rules.zig.

#include <stdint.h>

#include "driver/temperature_sensor.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static temperature_sensor_handle_t tsens;

void tinyblok_sources_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    temperature_sensor_install(&cfg, &tsens);
    temperature_sensor_enable(tsens);
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
