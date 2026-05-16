#include "tinyblok_patchbay.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "patchbay/pb_eval_internal.h"
#include "patchbay/pb_json.h"
#include "patchbay/pb_program.h"
#include "tinyblok_tx_ring.h"

#include "tinyblok_patchbay_edn.h"

#define MAIN_LOOP_DELAY_TICKS 10
#define MAX_PUMPS 16
#define MAX_FUNCTIONS 16
#define MAX_REQUESTS 16
#define FUNCTION_OUT_MAX 64

typedef uint32_t (*read_u32_fn)(void);
typedef int (*read_i32_fn)(void);
typedef float (*read_f32_fn)(void);
typedef uint64_t (*read_u64_fn)(void);
typedef float (*call_f32_f32_fn)(float);
typedef size_t (*call_bytes_fn)(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len);

typedef enum tinyblok_value_type
{
    TINYBLOK_TYPE_U32,
    TINYBLOK_TYPE_I32,
    TINYBLOK_TYPE_F32,
    TINYBLOK_TYPE_U64,
    TINYBLOK_TYPE_UPTIME_S,
    TINYBLOK_TYPE_BYTES,
} tinyblok_value_type;

// Compiled C symbol that a parsed `(pump ...)` or `(fn ...)` may reference.
typedef struct native_symbol
{
    const char *name;
    tinyblok_value_type type;
    tinyblok_value_type input;
    bool has_input;
    union
    {
        read_u32_fn read_u32;
        read_i32_fn read_i32;
        read_f32_fn read_f32;
        read_u64_fn read_u64;
        call_f32_f32_fn call_f32_f32;
        call_bytes_fn call_bytes;
    } fn;
} native_symbol;

// User-visible `(fn NAME ...)` binding. The name is borrowed from the parse
// arena; the target points at a static native symbol entry.
typedef struct function_binding
{
    pb_slice name;
    const native_symbol *target;
} function_binding;

// Request handler loaded from `(on-req SUBJECT BODY)`. Subject/body are owned
// by the declaration parse arena.
typedef struct request_binding
{
    pb_slice subject;
    pb_value body;
} request_binding;

typedef struct patchbay_runtime
{
    pb_program program;
    pb_arena decl_arena;
    tinyblok_pump pumps[MAX_PUMPS];
    const native_symbol *pump_symbols[MAX_PUMPS];
    tinyblok_value_type pump_types[MAX_PUMPS];
    size_t pump_count;
    function_binding functions[MAX_FUNCTIONS];
    size_t function_count;
    request_binding requests[MAX_REQUESTS];
    size_t request_count;
    mb_router router;
    pb_slice reply_subject;
} patchbay_runtime;

extern uint32_t tinyblok_free_heap(void);
extern int tinyblok_wifi_rssi(void);
extern uint64_t tinyblok_uptime_us(void);
extern float tinyblok_read_temp_c(void);
extern uint32_t tinyblok_user_counter(void);
extern size_t tinyblok_hello_c(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len);
extern ssize_t tinyblok_nats_try_send(const unsigned char *data, size_t len);
extern void tinyblok_nats_maintain(void);
extern void tinyblok_nats_drain_rx(void);
extern int tinyblok_nats_reply(const unsigned char *subject, size_t subject_len,
                               const unsigned char *payload, size_t payload_len);
extern void tinyblok_clock_arm(uint64_t us_until);

static const char *TAG = "patchbay";
static patchbay_runtime s_pb;

static bool tinyblok_eval_symbol(void *user_ctx, pb_eval_ctx *ctx, pb_slice name, pb_eval_result *out);
static bool tinyblok_eval_call(void *user_ctx, pb_eval_ctx *ctx, pb_slice name, pb_values args, pb_eval_result *out);

static size_t tinyblok_hello_c17(const uint8_t *payload, size_t payload_len, uint8_t *out, size_t out_len)
{
    static const char prefix[] = "hello from c17: ";
    size_t n = 0;
    for (size_t i = 0; i < sizeof(prefix) - 1 && n < out_len; i += 1)
        out[n++] = (uint8_t)prefix[i];
    for (size_t i = 0; i < payload_len && n < out_len; i += 1)
        out[n++] = payload[i];
    return n;
}

static const native_symbol NATIVE_SYMBOLS[] = {
    {.name = "tinyblok_free_heap", .type = TINYBLOK_TYPE_U32, .fn.read_u32 = tinyblok_free_heap},
    {.name = "tinyblok_wifi_rssi", .type = TINYBLOK_TYPE_I32, .fn.read_i32 = tinyblok_wifi_rssi},
    {.name = "tinyblok_uptime_us", .type = TINYBLOK_TYPE_U64, .fn.read_u64 = tinyblok_uptime_us},
    {.name = "tinyblok_read_temp_c", .type = TINYBLOK_TYPE_F32, .fn.read_f32 = tinyblok_read_temp_c},
    {.name = "tinyblok_user_counter", .type = TINYBLOK_TYPE_U32, .fn.read_u32 = tinyblok_user_counter},
    {.name = "tinyblok_hello_c", .type = TINYBLOK_TYPE_BYTES, .input = TINYBLOK_TYPE_BYTES, .has_input = true, .fn.call_bytes = tinyblok_hello_c},
    {.name = "tinyblok_hello_c17", .type = TINYBLOK_TYPE_BYTES, .input = TINYBLOK_TYPE_BYTES, .has_input = true, .fn.call_bytes = tinyblok_hello_c17},
};

static bool slice_eq(pb_slice s, const char *lit)
{
    const size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static const native_symbol *find_native(pb_slice name)
{
    for (size_t i = 0; i < sizeof(NATIVE_SYMBOLS) / sizeof(NATIVE_SYMBOLS[0]); i += 1)
    {
        if (slice_eq(name, NATIVE_SYMBOLS[i].name))
            return &NATIVE_SYMBOLS[i];
    }
    return NULL;
}

static function_binding *find_function(pb_slice name)
{
    for (size_t i = 0; i < s_pb.function_count; i += 1)
    {
        if (s_pb.functions[i].name.len == name.len && memcmp(s_pb.functions[i].name.ptr, name.ptr, name.len) == 0)
            return &s_pb.functions[i];
    }
    return NULL;
}

static bool type_from_value(pb_value v, tinyblok_value_type *out)
{
    if (v.kind != PB_SYMBOL && v.kind != PB_KEYWORD)
        return false;
    if (slice_eq(v.text, "u32"))
        *out = TINYBLOK_TYPE_U32;
    else if (slice_eq(v.text, "i32"))
        *out = TINYBLOK_TYPE_I32;
    else if (slice_eq(v.text, "f32") || slice_eq(v.text, "float"))
        *out = TINYBLOK_TYPE_F32;
    else if (slice_eq(v.text, "u64"))
        *out = TINYBLOK_TYPE_U64;
    else if (slice_eq(v.text, "uptime-s"))
        *out = TINYBLOK_TYPE_UPTIME_S;
    else if (slice_eq(v.text, "bytes") || slice_eq(v.text, "byte"))
        *out = TINYBLOK_TYPE_BYTES;
    else
        return false;
    return true;
}

static bool kwarg(pb_values items, const char *key, pb_value *out)
{
    for (size_t i = 2; i + 1 < items.len; i += 2)
    {
        if (items.items[i].kind == PB_KEYWORD && slice_eq(items.items[i].text, key))
        {
            *out = items.items[i + 1];
            return true;
        }
    }
    return false;
}

static bool number_to_u64(pb_value v, uint64_t *out)
{
    if (v.kind != PB_NUMBER || v.number < 0 || floor(v.number) != v.number)
        return false;
    *out = (uint64_t)v.number;
    return true;
}

static char *arena_cstr(pb_slice s)
{
    char *out = pb_arena_alloc(&s_pb.decl_arena, s.len + 1, 1);
    if (out == NULL)
        return NULL;
    memcpy(out, s.ptr, s.len);
    out[s.len] = '\0';
    return out;
}

static void bridge_publish(void *ctx, mb_slice subject, mb_slice payload)
{
    (void)ctx;
    (void)tinyblok_tx_ring_enqueue(subject.ptr, subject.len, payload.ptr, payload.len);
}

static double read_native_number(const native_symbol *sym)
{
    switch (sym->type)
    {
    case TINYBLOK_TYPE_U32:
        return (double)sym->fn.read_u32();
    case TINYBLOK_TYPE_I32:
        return (double)sym->fn.read_i32();
    case TINYBLOK_TYPE_F32:
        return (double)sym->fn.read_f32();
    case TINYBLOK_TYPE_U64:
        return (double)sym->fn.read_u64();
    case TINYBLOK_TYPE_UPTIME_S:
        return (double)sym->fn.read_u64() / 1000000.0;
    case TINYBLOK_TYPE_BYTES:
        break;
    }
    return 0.0;
}

static bool format_number(char *buf, size_t cap, tinyblok_value_type type, double value, size_t *out_len)
{
    int n = -1;
    switch (type)
    {
    case TINYBLOK_TYPE_U32:
    case TINYBLOK_TYPE_U64:
        n = snprintf(buf, cap, "%.0f", value);
        break;
    case TINYBLOK_TYPE_I32:
        n = snprintf(buf, cap, "%d", (int)value);
        break;
    case TINYBLOK_TYPE_F32:
        n = snprintf(buf, cap, "%.7f", value);
        break;
    case TINYBLOK_TYPE_UPTIME_S:
        n = snprintf(buf, cap, "%.3f", value);
        break;
    case TINYBLOK_TYPE_BYTES:
        return false;
    }
    if (n <= 0 || (size_t)n >= cap)
        return false;
    *out_len = (size_t)n;
    return true;
}

static bool eval_publish_bytes(pb_slice subject, pb_slice payload)
{
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject.ptr, .len = subject.len};
    const mb_slice mb_payload = {.ptr = (const uint8_t *)payload.ptr, .len = payload.len};

    const bool published = mb_router_publish(&s_pb.router, mb_subject, mb_payload);
    const bool ok = pb_program_eval_publish(&s_pb.program, &s_pb.router, mb_subject, mb_payload, now_ms, (int64_t)now_ms);
    tinyblok_patchbay_arm_next_clock();
    return published && ok;
}

static bool load_pump(pb_values items)
{
    if (items.len < 8 || s_pb.pump_count >= MAX_PUMPS || items.items[1].kind != PB_STRING)
        return false;

    pb_value from = {0};
    pb_value type = {0};
    pb_value hz = {0};
    uint64_t hz_u64 = 0;
    tinyblok_value_type declared_type = {0};
    if (!kwarg(items, "from", &from) || !kwarg(items, "type", &type) || !kwarg(items, "hz", &hz) ||
        from.kind != PB_SYMBOL || !type_from_value(type, &declared_type) || !number_to_u64(hz, &hz_u64) || hz_u64 == 0)
        return false;

    const native_symbol *sym = find_native(from.text);
    if (sym == NULL)
        return false;
    char *subject = arena_cstr(items.items[1].text);
    if (subject == NULL)
        return false;

    const size_t i = s_pb.pump_count;
    s_pb.pumps[i] = (tinyblok_pump){
        .subject = subject,
        .period_us = 1000000ULL / hz_u64,
    };
    s_pb.pump_symbols[i] = sym;
    s_pb.pump_types[i] = declared_type;
    s_pb.pump_count += 1;
    return true;
}

static bool load_function(pb_values items)
{
    if (items.len < 6 || s_pb.function_count >= MAX_FUNCTIONS || items.items[1].kind != PB_SYMBOL)
        return false;

    pb_value from = {0};
    if (!kwarg(items, "from", &from) || from.kind != PB_SYMBOL)
        return false;
    const native_symbol *sym = find_native(from.text);
    if (sym == NULL)
        return false;

    s_pb.functions[s_pb.function_count++] = (function_binding){
        .name = items.items[1].text,
        .target = sym,
    };
    return true;
}

static bool load_request(pb_values items)
{
    if (items.len < 3 || s_pb.request_count >= MAX_REQUESTS || items.items[1].kind != PB_STRING)
        return false;
    char *subject = arena_cstr(items.items[1].text);
    if (subject == NULL)
        return false;
    s_pb.requests[s_pb.request_count++] = (request_binding){
        .subject = {.ptr = subject, .len = items.items[1].text.len},
        .body = items.items[items.len - 1],
    };
    return true;
}

static bool load_declarations(void)
{
    const pb_parse_result parsed = pb_parse_patchbay_source(&s_pb.decl_arena, "patchbay.edn",
                                                            (const char *)tinyblok_patchbay_edn,
                                                            (size_t)tinyblok_patchbay_edn_len);
    if (parsed.err != PB_PARSE_OK)
    {
        ESP_LOGE(TAG, "parse error: %s at byte %u", pb_parse_error_name(parsed.err), (unsigned)parsed.err_offset);
        return false;
    }

    for (size_t i = 0; i < parsed.forms.len; i += 1)
    {
        pb_value form = parsed.forms.items[i];
        if (form.kind != PB_LIST || form.seq.len == 0 || form.seq.items[0].kind != PB_SYMBOL)
            continue;
        if (slice_eq(form.seq.items[0].text, "pump") && !load_pump(form.seq))
            return false;
        if (slice_eq(form.seq.items[0].text, "fn") && !load_function(form.seq))
            return false;
        if (slice_eq(form.seq.items[0].text, "on-req") && !load_request(form.seq))
            return false;
    }
    return true;
}

int tinyblok_patchbay_init(void)
{
    memset(&s_pb, 0, sizeof(s_pb));
    mb_router_init(&s_pb.router);
    s_pb.router.bridge_ctx = &s_pb;
    s_pb.router.bridge_fn = bridge_publish;
    if (!pb_program_load_source(&s_pb.program, "patchbay.edn", (const char *)tinyblok_patchbay_edn, (size_t)tinyblok_patchbay_edn_len))
        return -1;
    if (!load_declarations())
        return -1;
    pb_program_set_eval_hooks(&s_pb.program, tinyblok_eval_symbol, tinyblok_eval_call, &s_pb);
    ESP_LOGI(TAG, "loaded %u rule(s), %u pump(s), %u request(s)",
             (unsigned)s_pb.program.len, (unsigned)s_pb.pump_count, (unsigned)s_pb.request_count);
    return 0;
}

void tinyblok_patchbay_main(void)
{
    for (;;)
    {
        tinyblok_nats_maintain();
        tinyblok_tx_ring_drain(tinyblok_nats_try_send);
        tinyblok_nats_drain_rx();
        vTaskDelay(MAIN_LOOP_DELAY_TICKS);
    }
}

size_t tinyblok_patchbay_pump_count(void)
{
    return s_pb.pump_count;
}

const tinyblok_pump *tinyblok_patchbay_pump(size_t index)
{
    return index < s_pb.pump_count ? &s_pb.pumps[index] : NULL;
}

void tinyblok_patchbay_fire_pump(size_t index)
{
    if (index >= s_pb.pump_count)
        return;
    const native_symbol *sym = s_pb.pump_symbols[index];
    char payload[TINYBLOK_PAYLOAD_MAX];
    size_t payload_len = 0;
    double value = read_native_number(sym);
    if (s_pb.pump_types[index] == TINYBLOK_TYPE_UPTIME_S)
        value = value / 1000000.0;
    if (!format_number(payload, sizeof(payload), s_pb.pump_types[index], value, &payload_len))
        return;
    const pb_slice subject = {.ptr = s_pb.pumps[index].subject, .len = strlen(s_pb.pumps[index].subject)};
    const pb_slice body = {.ptr = payload, .len = payload_len};
    (void)eval_publish_bytes(subject, body);
}

size_t tinyblok_patchbay_request_count(void)
{
    return s_pb.request_count;
}

const char *tinyblok_patchbay_request_subject(size_t index)
{
    return index < s_pb.request_count ? s_pb.requests[index].subject.ptr : NULL;
}

static bool tinyblok_eval_symbol(void *user_ctx, pb_eval_ctx *ctx, pb_slice name, pb_eval_result *out)
{
    (void)user_ctx;
    (void)ctx;
    if (slice_eq(name, "uptime-s"))
    {
        *out = ok((pb_value){.kind = PB_NUMBER, .number = (double)tinyblok_uptime_us() / 1000000.0});
        return true;
    }
    if (slice_eq(name, "uptime-us"))
    {
        *out = ok((pb_value){.kind = PB_NUMBER, .number = (double)tinyblok_uptime_us()});
        return true;
    }
    function_binding *binding = find_function(name);
    if (binding == NULL || binding->target->has_input)
        return false;
    *out = ok((pb_value){.kind = PB_NUMBER, .number = read_native_number(binding->target)});
    return true;
}

static bool tinyblok_eval_call(void *user_ctx, pb_eval_ctx *ctx, pb_slice name, pb_values args, pb_eval_result *out)
{
    (void)user_ctx;
    if (slice_eq(name, "reply!") || slice_eq(name, "reply"))
    {
        if (args.len > 1)
        {
            *out = fail(PB_EVAL_ARITY);
            return true;
        }
        pb_value value = args.len == 0 ? (pb_value){.kind = PB_STRING, .text = ctx->payload} : args.items[0];
        pb_slice payload = {0};
        if (!coerce_payload(ctx, value, &payload))
        {
            *out = fail(PB_EVAL_TYPE);
            return true;
        }
        if (tinyblok_nats_reply((const unsigned char *)s_pb.reply_subject.ptr, s_pb.reply_subject.len,
                                (const unsigned char *)payload.ptr, payload.len) != 0)
        {
            *out = fail(PB_EVAL_PUBLISH_FAILED);
            return true;
        }
        *out = ok((pb_value){.kind = PB_NIL});
        return true;
    }

    function_binding *binding = find_function(name);
    if (binding == NULL)
        return false;
    const native_symbol *sym = binding->target;
    if (!sym->has_input)
    {
        if (args.len != 0)
        {
            *out = fail(PB_EVAL_ARITY);
            return true;
        }
        *out = ok((pb_value){.kind = PB_NUMBER, .number = read_native_number(sym)});
        return true;
    }
    if (sym->input == TINYBLOK_TYPE_BYTES && sym->type == TINYBLOK_TYPE_BYTES)
    {
        if (args.len != 1)
        {
            *out = fail(PB_EVAL_ARITY);
            return true;
        }
        pb_slice in = {0};
        if (!as_string(args.items[0], &in))
        {
            *out = fail(PB_EVAL_TYPE);
            return true;
        }
        uint8_t *buf = pb_arena_alloc(ctx->arena, FUNCTION_OUT_MAX, 1);
        if (buf == NULL)
        {
            *out = fail(PB_EVAL_OOM);
            return true;
        }
        const size_t n = sym->fn.call_bytes((const uint8_t *)in.ptr, in.len, buf, FUNCTION_OUT_MAX);
        *out = ok((pb_value){.kind = PB_STRING, .text = {.ptr = (const char *)buf, .len = n <= FUNCTION_OUT_MAX ? n : FUNCTION_OUT_MAX}});
        return true;
    }

    *out = fail(PB_EVAL_TYPE);
    return true;
}

void tinyblok_patchbay_handle_msg(const unsigned char *subject, size_t subject_len,
                                  const unsigned char *reply, size_t reply_len,
                                  const unsigned char *payload, size_t payload_len)
{
    for (size_t i = 0; i < s_pb.request_count; i += 1)
    {
        request_binding *req = &s_pb.requests[i];
        if (req->subject.len != subject_len || memcmp(req->subject.ptr, subject, subject_len) != 0)
            continue;

        pb_arena scratch = {0};
        pb_eval_state state = {0};
        s_pb.reply_subject = (pb_slice){.ptr = (const char *)reply, .len = reply_len};
        pb_eval_ctx ctx = {
            .arena = &scratch,
            .state = &state,
            .subject = {.ptr = (const char *)subject, .len = subject_len},
            .payload = {.ptr = (const char *)payload, .len = payload_len},
            .user_symbol = tinyblok_eval_symbol,
            .user_call = tinyblok_eval_call,
            .user_ctx = &s_pb,
        };
        const pb_eval_result r = pb_eval(&ctx, req->body);
        if (r.err != PB_EVAL_OK)
            ESP_LOGW(TAG, "request eval failed: %s", pb_eval_error_name(r.err));
        s_pb.reply_subject = (pb_slice){0};
        pb_eval_state_free(&state);
        pb_arena_free(&scratch);
        return;
    }
}

void tinyblok_patchbay_clock_fired(void)
{
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    (void)pb_program_tick(&s_pb.program, &s_pb.router, now_ms, (int64_t)now_ms);
    tinyblok_patchbay_arm_next_clock();
}

void tinyblok_patchbay_arm_next_clock(void)
{
    uint64_t deadline_ms = 0;
    if (!pb_program_next_clock_deadline(&s_pb.program, &deadline_ms))
        return;
    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    const uint64_t us_until = deadline_ms > now_ms ? (deadline_ms - now_ms) * 1000ULL : 1;
    tinyblok_clock_arm(us_until);
}
