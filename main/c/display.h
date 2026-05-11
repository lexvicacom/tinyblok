#pragma once

#include <stdint.h>

#include "esp_netif_ip_addr.h"

void tinyblok_display_start(void);
void tinyblok_display_wifi_connecting(const char *ssid);
void tinyblok_display_wifi_connected(const char *ssid, const esp_ip4_addr_t *ip);
void tinyblok_display_wifi_disconnected(void);
