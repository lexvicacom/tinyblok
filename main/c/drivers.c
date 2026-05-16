// Parsed pumps arm esp_timer instances that enqueue bounded driver events.
// Clocked patchbay work shares one one-shot timer; the runtime computes the
// next deadline from long-lived evaluator state.

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "tinyblok_patchbay.h"

typedef struct driver_event
{
    size_t source_index;
} driver_event;

static const char *TAG = "drivers";

static QueueHandle_t s_driver_queue;
static TaskHandle_t s_driver_task;
static esp_timer_handle_t s_clock_handle;
static atomic_uint_fast32_t s_driver_event_drops;

int64_t tinyblok_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void handle_driver_event(driver_event event)
{
    const size_t i = event.source_index;
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

static void driver_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        driver_event event = {0};
        if (xQueueReceive(s_driver_queue, &event, portMAX_DELAY) != pdTRUE)
            continue;

        const uint32_t drops = (uint32_t)atomic_exchange_explicit(&s_driver_event_drops, 0, memory_order_relaxed);
        if (drops > 0)
            ESP_LOGW(TAG, "dropped %u driver event(s)", (unsigned)drops);

        handle_driver_event(event);
    }
}

static void timer_cb(void *arg)
{
    driver_event event = {.source_index = (size_t)(intptr_t)arg};
    if (s_driver_queue == NULL || xQueueSend(s_driver_queue, &event, 0) != pdTRUE)
        atomic_fetch_add_explicit(&s_driver_event_drops, 1, memory_order_relaxed);
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
    s_driver_queue = xQueueCreate(CONFIG_TINYBLOK_DRIVER_EVENT_QUEUE_LEN, sizeof(driver_event));
    ESP_ERROR_CHECK(s_driver_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    BaseType_t task_ok = xTaskCreate(driver_task, "tinyblok_driver",
                                     CONFIG_TINYBLOK_DRIVER_TASK_STACK_SIZE,
                                     NULL, tskIDLE_PRIORITY + 4, &s_driver_task);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

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
