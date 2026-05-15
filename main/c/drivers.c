// Parsed pumps arm esp_timer instances that post to TINYBLOK_EVENTS. Clocked
// patchbay work shares one one-shot timer; the runtime computes the next
// deadline from long-lived evaluator state.

#include <stdint.h>
#include <stddef.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tinyblok_patchbay.h"

ESP_EVENT_DEFINE_BASE(TINYBLOK_EVENTS);

static const char *TAG = "drivers";

int64_t tinyblok_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static esp_timer_handle_t s_clock_handle;

static void on_pump_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    const size_t i = (size_t)id;
    const size_t pump_count = tinyblok_patchbay_pump_count();
    if (i < pump_count)
    {
        tinyblok_patchbay_fire_pump(i);
        return;
    }
    if (i == pump_count)
    {
        tinyblok_patchbay_clock_fired();
        return;
    }
}

static void timer_cb(void *arg)
{
    intptr_t id = (intptr_t)arg;
    esp_event_post(TINYBLOK_EVENTS, (int32_t)id, NULL, 0, 0);
}

void tinyblok_clock_arm(uint64_t us_until)
{
    if (s_clock_handle == NULL)
        return;
    esp_timer_stop(s_clock_handle);
    ESP_ERROR_CHECK(esp_timer_start_once(s_clock_handle, us_until));
}

void tinyblok_drivers_start(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(TINYBLOK_EVENTS, ESP_EVENT_ANY_ID,
                                               &on_pump_event, NULL));

    const size_t pump_count = tinyblok_patchbay_pump_count();
    for (size_t i = 0; i < pump_count; i++)
    {
        const tinyblok_pump *p = tinyblok_patchbay_pump(i);
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

    const esp_timer_create_args_t args = {
        .callback = &timer_cb,
        .arg = (void *)(intptr_t)pump_count,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "tinyblok-clock",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_clock_handle));
}
