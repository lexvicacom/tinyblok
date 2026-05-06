// Each generated pump arms one esp_timer that posts to TINYBLOK_EVENTS.
// on_pump_event runs on the esp_event task and calls into the Zig-side
// pump table; the main loop only services network I/O.
//
// Time-windowed `bar! :ms` slots use the same dispatch path: at boot we
// create one esp_timer per generated `tinyblok_clock_slots[]` entry,
// initially not armed. The Zig dispatcher and per-slot fire fn each call
// `tinyblok_clock_arm(slot_id, us)` to schedule a one-shot timer at the
// slot's exact next deadline (computed by the kernel's `nextDeadlineMs`),
// replacing the previous periodic walker.

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

typedef struct
{
    void (*fire)(void);
} tinyblok_clock_slot_t;

extern const size_t tinyblok_pump_count;
extern const tinyblok_pump_t tinyblok_pumps[];
extern const size_t tinyblok_clock_slot_count;
extern const tinyblok_clock_slot_t tinyblok_clock_slots[];

int64_t tinyblok_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Per-clock-slot esp_timer handle. Created at boot (`tinyblok_drivers_start`),
// armed on demand by `tinyblok_clock_arm`. Sized to a small upper bound so
// we don't need a heap allocation; bumping is cheap if you ever generate
// more time bars than this.
#define TINYBLOK_MAX_CLOCK_SLOTS 32
static esp_timer_handle_t s_clock_handles[TINYBLOK_MAX_CLOCK_SLOTS];

static void on_pump_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    const size_t i = (size_t)id;
    if (i < tinyblok_pump_count)
    {
        tinyblok_pumps[i].fire();
        return;
    }
    const size_t clock_i = i - tinyblok_pump_count;
    if (clock_i < tinyblok_clock_slot_count)
    {
        tinyblok_clock_slots[clock_i].fire();
        return;
    }
}

static void timer_cb(void *arg)
{
    intptr_t id = (intptr_t)arg;
    esp_event_post(TINYBLOK_EVENTS, (int32_t)id, NULL, 0, 0);
}

void tinyblok_clock_arm(size_t slot_id, uint64_t us_until)
{
    if (slot_id >= tinyblok_clock_slot_count)
        return;
    esp_timer_handle_t h = s_clock_handles[slot_id];
    if (h == NULL)
        return;
    // esp_timer_start_once errors if the timer is already running. Stop
    // first so reschedules during an in-flight period replace cleanly.
    // ESP_ERR_INVALID_STATE from a not-running timer is fine to ignore.
    esp_timer_stop(h);
    ESP_ERROR_CHECK(esp_timer_start_once(h, us_until));
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

    if (tinyblok_clock_slot_count > TINYBLOK_MAX_CLOCK_SLOTS)
    {
        ESP_LOGE(TAG, "tinyblok_clock_slot_count=%u exceeds TINYBLOK_MAX_CLOCK_SLOTS=%d; bump the cap",
                 (unsigned)tinyblok_clock_slot_count, TINYBLOK_MAX_CLOCK_SLOTS);
        abort();
    }
    for (size_t i = 0; i < tinyblok_clock_slot_count; i++)
    {
        const intptr_t event_id = (intptr_t)(tinyblok_pump_count + i);
        const esp_timer_create_args_t args = {
            .callback = &timer_cb,
            .arg = (void *)event_id,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "tinyblok-clock",
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_clock_handles[i]));
        // No initial arm; the Zig dispatcher arms each slot on the first
        // PUB that opens its bar.
    }
    if (tinyblok_clock_slot_count > 0)
    {
        ESP_LOGI(TAG, "clock registry: %u slot(s) ready",
                 (unsigned)tinyblok_clock_slot_count);
    }
}
