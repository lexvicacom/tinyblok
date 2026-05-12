#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t tinyblok_wifi_start_setup_ap(void);
esp_err_t tinyblok_wifi_stop_setup_ap(void);
esp_err_t tinyblok_wifi_connect_sta(const char *ssid, const char *password, uint32_t timeout_ms);
bool tinyblok_wifi_is_connected(void);
esp_err_t tinyblok_wifi_get_ip_string(char *buf, size_t len);
