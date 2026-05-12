#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char device_name[64];
    char wifi_ssid[64];
    char wifi_password[128];

    char nats_host[128];
    uint16_t nats_port;
    char nats_user[64];
    char nats_password[128];
    char nats_token[256];
    char nats_base_topic[128];
    char nats_auth[16];
    bool nats_tls;
    char nats_creds[4096];

    bool display_enabled;
    uint8_t display_brightness;

    uint32_t sample_interval_ms;
    char timezone[64];

    bool configured;
} tinyblok_config_t;

esp_err_t tinyblok_config_init(void);
esp_err_t tinyblok_config_load(tinyblok_config_t *cfg);
esp_err_t tinyblok_config_save(const tinyblok_config_t *cfg);
esp_err_t tinyblok_config_reset(void);
void tinyblok_config_load_defaults(tinyblok_config_t *cfg);
