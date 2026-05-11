#include "app_events.h"

#include <stdatomic.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_log.h"
#endif

static atomic_uint_fast32_t message_count;
static atomic_uint_fast32_t pub_count;

#ifdef ESP_PLATFORM
ESP_EVENT_DEFINE_BASE(TINYBLOK_EVENT);

static const char *TAG = "events";
#endif

void tinyblok_events_init(void)
{
}

uint32_t tinyblok_message_count(void)
{
    return (uint32_t)atomic_load_explicit(&message_count, memory_order_relaxed);
}

uint32_t tinyblok_pub_count(void)
{
    return (uint32_t)atomic_load_explicit(&pub_count, memory_order_relaxed);
}

void tinyblok_event_publish_message_processed(void)
{
    atomic_fetch_add_explicit(&message_count, 1, memory_order_relaxed);
#ifdef ESP_PLATFORM
    esp_err_t err = esp_event_post(TINYBLOK_EVENT, TINYBLOK_EVENT_MESSAGE_PROCESSED,
                                   NULL, 0, 0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "message event post failed: %s", esp_err_to_name(err));
#endif
}

void tinyblok_event_publish_pub_sent(void)
{
    atomic_fetch_add_explicit(&pub_count, 1, memory_order_relaxed);
#ifdef ESP_PLATFORM
    esp_err_t err = esp_event_post(TINYBLOK_EVENT, TINYBLOK_EVENT_PUB_SENT, NULL, 0, 0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "pub event post failed: %s", esp_err_to_name(err));
#endif
}

void tinyblok_event_publish_nats_connected(const char *host, const char *ip, uint16_t port)
{
#ifdef ESP_PLATFORM
    tinyblok_nats_connected_event_t event = {0};
    if (host != NULL)
        strlcpy(event.host, host, sizeof(event.host));
    if (ip != NULL)
        strlcpy(event.ip, ip, sizeof(event.ip));
    event.port = port;

    esp_err_t err = esp_event_post(TINYBLOK_EVENT, TINYBLOK_EVENT_NATS_CONNECTED,
                                   &event, sizeof(event), 0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "nats event post failed: %s", esp_err_to_name(err));
#else
    (void)host;
    (void)ip;
    (void)port;
#endif
}

void tinyblok_event_publish_nats_disconnected(void)
{
#ifdef ESP_PLATFORM
    esp_err_t err = esp_event_post(TINYBLOK_EVENT, TINYBLOK_EVENT_NATS_DISCONNECTED,
                                   NULL, 0, 0);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "nats disconnect event post failed: %s", esp_err_to_name(err));
#endif
}
