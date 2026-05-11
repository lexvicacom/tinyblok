#pragma once

#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_event.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    TINYBLOK_EVENT_MESSAGE_PROCESSED = 1,
    TINYBLOK_EVENT_NATS_CONNECTED = 2,
    TINYBLOK_EVENT_PUB_SENT = 3,
    TINYBLOK_EVENT_NATS_DISCONNECTED = 4,
} tinyblok_event_id_t;

typedef struct
{
    char host[64];
    char ip[16];
    uint16_t port;
} tinyblok_nats_connected_event_t;

#ifdef ESP_PLATFORM
ESP_EVENT_DECLARE_BASE(TINYBLOK_EVENT);
#endif

void tinyblok_events_init(void);
void tinyblok_event_publish_message_processed(void);
void tinyblok_event_publish_pub_sent(void);
void tinyblok_event_publish_nats_connected(const char *host, const char *ip, uint16_t port);
void tinyblok_event_publish_nats_disconnected(void);
uint32_t tinyblok_message_count(void);
uint32_t tinyblok_pub_count(void);

#ifdef __cplusplus
}
#endif
