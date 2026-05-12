#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TINYBLOK_WIFI_SCAN_MAX 24

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} tinyblok_wifi_scan_result_t;

esp_err_t tinyblok_wifi_start_setup_ap(void);
esp_err_t tinyblok_wifi_stop_setup_ap(void);
esp_err_t tinyblok_wifi_connect_sta(const char *ssid, const char *password, uint32_t timeout_ms);
esp_err_t tinyblok_wifi_scan(tinyblok_wifi_scan_result_t *results, size_t cap, size_t *count);
bool tinyblok_wifi_is_connected(void);
esp_err_t tinyblok_wifi_get_ip_string(char *buf, size_t len);
