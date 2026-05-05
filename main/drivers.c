// Each generated pump arms one esp_timer that posts to TINYBLOK_EVENTS.
// on_pump_event runs on the esp_event task and calls into the Zig-side
// pump table; the main loop only services network I/O.

#include <stdint.h>
#include <stddef.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

ESP_EVENT_DEFINE_BASE(TINYBLOK_EVENTS);

static const char *TAG = "drivers";

typedef struct
{
    const char *subject;
    uint64_t period_us;
    void (*fire)(void);
} tinyblok_pump_t;

extern const size_t tinyblok_pump_count;
extern const tinyblok_pump_t tinyblok_pumps[];

static void on_pump_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if ((size_t)id >= tinyblok_pump_count)
        return;
    tinyblok_pumps[id].fire();
}

static void timer_cb(void *arg)
{
    intptr_t id = (intptr_t)arg;
    esp_event_post(TINYBLOK_EVENTS, (int32_t)id, NULL, 0, 0);
}

void tinyblok_drivers_start(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(TINYBLOK_EVENTS, ESP_EVENT_ANY_ID,
                                               &on_pump_event, NULL));

    for (size_t i = 0; i < tinyblok_pump_count; i++)
    {
        const tinyblok_pump_t *p = &tinyblok_pumps[i];
        esp_timer_handle_t t;
        const esp_timer_create_args_t args = {
            .callback = &timer_cb,
            .arg = (void *)(intptr_t)i,
            .dispatch_method = ESP_TIMER_TASK,
            .name = p->subject,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &t));
        ESP_ERROR_CHECK(esp_timer_start_periodic(t, p->period_us));
        ESP_LOGI(TAG, "pump '%s' armed @ %llu us", p->subject,
                 (unsigned long long)p->period_us);
    }
}
